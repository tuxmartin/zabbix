/*
** Zabbix
** Copyright (C) 2001-2020 Zabbix SIA
**
** This program is free software; you can redistribute it and/or modify
** it under the terms of the GNU General Public License as published by
** the Free Software Foundation; either version 2 of the License, or
** (at your option) any later version.
**
** This program is distributed in the hope that it will be useful,
** but WITHOUT ANY WARRANTY; without even the implied warranty of
** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
** GNU General Public License for more details.
**
** You should have received a copy of the GNU General Public License
** along with this program; if not, write to the Free Software
** Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
**/

#include "common.h"
#include "log.h"
#include "zbxalgo.h"
#include "zbxserver.h"
#include "eval.h"

/* exit code in addition to SUCCEED/FAIL */
#define UNKNOWN		1

/* bit function types */
typedef enum
{
	FUNCTION_OPTYPE_BIT_AND = 0,
	FUNCTION_OPTYPE_BIT_OR,
	FUNCTION_OPTYPE_BIT_XOR,
	FUNCTION_OPTYPE_BIT_LSHIFT,
	FUNCTION_OPTYPE_BIT_RSHIFT
}
zbx_function_bit_optype_t;

/* trim function types */
typedef enum
{
	FUNCTION_OPTYPE_TRIM_ALL = 0,
	FUNCTION_OPTYPE_TRIM_LEFT,
	FUNCTION_OPTYPE_TRIM_RIGHT
}
zbx_function_trim_optype_t;

/******************************************************************************
 *                                                                            *
 * Function: variant_convert_suffixed_num                                     *
 *                                                                            *
 * Purpose: convert variant string value containing suffixed number to        *
 *          floating point variant value                                      *
 *                                                                            *
 * Parameters: value     - [OUT] the output value                             *
 *             value_num - [IN] the value to convert                          *
 *                                                                            *
 * Return value: SUCCEED - the value was converted successfully               *
 *               FAIL    - otherwise                                          *
 *                                                                            *
 ******************************************************************************/
static int	variant_convert_suffixed_num(zbx_variant_t *value, const zbx_variant_t *value_num)
{
	char	suffix;

	if (ZBX_VARIANT_STR != value_num->type)
		return FAIL;

	if (SUCCEED != eval_suffixed_number_parse(value_num->data.str, &suffix))
		return FAIL;

	zbx_variant_set_dbl(value, atof(value_num->data.str) * suffix2factor(suffix));

	return SUCCEED;
}

/******************************************************************************
 *                                                                            *
 * Function: eval_execute_op_unary                                            *
 *                                                                            *
 * Purpose: evaluate unary operator                                           *
 *                                                                            *
 * Parameters: ctx      - [IN] the evaluation context                         *
 *             token    - [IN] the operator token                             *
 *             output   - [IN/OUT] the output value stack                     *
 *             error    - [OUT] the error message in the case of failure      *
 *                                                                            *
 * Return value: SUCCEED - the operator was evaluated successfully            *
 *               FAIL    - otherwise                                          *
 *                                                                            *
 ******************************************************************************/
static int	eval_execute_op_unary(const zbx_eval_context_t *ctx, const zbx_eval_token_t *token,
		zbx_vector_var_t *output, char **error)
{
	zbx_variant_t	*right;
	double		value;

	if (1 > output->values_num)
	{
		*error = zbx_dsprintf(*error, "unary operator requires one operand at \"%s\"",
				ctx->expression + token->loc.l);
		return FAIL;
	}

	right = &output->values[output->values_num - 1];

	if (ZBX_VARIANT_ERR == right->type)
		return SUCCEED;

	if (SUCCEED != zbx_variant_convert(right, ZBX_VARIANT_DBL))
	{
		*error = zbx_dsprintf(*error, "unary operator operand \"%s\" is not a numeric value at \"%s\"",
				zbx_variant_value_desc(right), ctx->expression + token->loc.l);
		return FAIL;
	}

	switch (token->type)
	{
		case ZBX_EVAL_TOKEN_OP_MINUS:
			value = -right->data.dbl;
			break;
		case ZBX_EVAL_TOKEN_OP_NOT:
			value = (SUCCEED == zbx_double_compare(right->data.dbl, 0) ? 1 : 0);
			break;
		default:
			THIS_SHOULD_NEVER_HAPPEN;
			*error = zbx_dsprintf(*error, "unknown unary operator at \"%s\"",
					ctx->expression + token->loc.l);
			return FAIL;
	}

	zbx_variant_clear(right);
	zbx_variant_set_dbl(right, value);

	return SUCCEED;
}

/******************************************************************************
 *                                                                            *
 * Function: eval_execute_op_logic_err                                        *
 *                                                                            *
 * Purpose: evaluate logical or/and operator with one operand being error     *
 *                                                                            *
 * Parameters: token  - [IN] the operator token                               *
 *             value  - [IN] the other operand                                *
 *             result - [OUT] the resulting value                             *
 *                                                                            *
 * Return value: SUCCEED - the oeprator was evaluated successfully            *
 *               FAIL    - otherwise                                          *
 *                                                                            *
 ******************************************************************************/
static int	eval_execute_op_logic_err(const zbx_eval_token_t *token, const zbx_variant_t *value, double *result)
{
	zbx_variant_t	value_dbl;

	if (ZBX_VARIANT_ERR == value->type)
		return FAIL;

	zbx_variant_copy(&value_dbl, value);
	if (SUCCEED != zbx_variant_convert(&value_dbl, ZBX_VARIANT_DBL))
	{
		zbx_variant_clear(&value_dbl);
		return FAIL;
	}

	switch (token->type)
	{
		case ZBX_EVAL_TOKEN_OP_AND:
			if (SUCCEED == zbx_double_compare(value_dbl.data.dbl, 0))
			{
				*result = 0;
				return SUCCEED;
			}
			break;
		case ZBX_EVAL_TOKEN_OP_OR:
			if (SUCCEED != zbx_double_compare(value_dbl.data.dbl, 0))
			{
				*result = 1;
				return SUCCEED;
			}
			break;
	}

	return FAIL;
}

/******************************************************************************
 *                                                                            *
 * Function: eval_variant_compare                                             *
 *                                                                            *
 * Purpose: compare two variant values supporting suffixed numbers            *
 *                                                                            *
 * Return value: <0 - the first value is less than the second                 *
 *               >0 - the first value is greater than the second              *
 *               0  - the values are equal                                    *
 *                                                                            *
 ******************************************************************************/
static int	eval_variant_compare(const zbx_variant_t *left, const zbx_variant_t *right)
{
	zbx_variant_t	val_l, val_r;
	int		ret;

	zbx_variant_set_none(&val_l);
	zbx_variant_set_none(&val_r);

	if (SUCCEED == variant_convert_suffixed_num(&val_l, left))
		left = &val_l;

	if (SUCCEED == variant_convert_suffixed_num(&val_r, right))
		right = &val_r;

	ret = zbx_variant_compare(left, right);

	zbx_variant_clear(&val_l);
	zbx_variant_clear(&val_r);

	return ret;
}

/******************************************************************************
 *                                                                            *
 * Function: eval_execute_op_binary                                           *
 *                                                                            *
 * Purpose: evaluate binary operator                                          *
 *                                                                            *
 * Parameters: ctx      - [IN] the evaluation context                         *
 *             token    - [IN] the operator token                             *
 *             output   - [IN/OUT] the output value stack                     *
 *             error    - [OUT] the error message in the case of failure      *
 *                                                                            *
 * Return value: SUCCEED - the oeprator was evaluated successfully            *
 *               FAIL    - otherwise                                          *
 *                                                                            *
 ******************************************************************************/
static int	eval_execute_op_binary(const zbx_eval_context_t *ctx, const zbx_eval_token_t *token,
		zbx_vector_var_t *output, char **error)
{
	zbx_variant_t	*left, *right;
	double		value;

	if (2 > output->values_num)
	{
		*error = zbx_dsprintf(*error, "binary operator requires two operands at \"%s\"",
				ctx->expression + token->loc.l);

		return FAIL;
	}

	left = &output->values[output->values_num - 2];
	right = &output->values[output->values_num - 1];

	/* process error operands */

	if (ZBX_VARIANT_ERR == left->type)
	{
		if (ZBX_EVAL_TOKEN_OP_AND == token->type || ZBX_EVAL_TOKEN_OP_OR == token->type)
		{
			if (SUCCEED == eval_execute_op_logic_err(token, right, &value))
				goto finish;
		}

		zbx_variant_clear(right);
		output->values_num--;

		return SUCCEED;
	}
	else if (ZBX_VARIANT_ERR == right->type)
	{
		if (ZBX_EVAL_TOKEN_OP_AND == token->type || ZBX_EVAL_TOKEN_OP_OR == token->type)
		{
			if (SUCCEED == eval_execute_op_logic_err(token, left, &value))
				goto finish;
		}
		zbx_variant_clear(left);
		*left = *right;
		output->values_num--;

		return SUCCEED;
	}

	/* check logical equal, not equal operators */

	switch (token->type)
	{
		case ZBX_EVAL_TOKEN_OP_EQ:
			value = (0 == eval_variant_compare(left, right) ? 1 : 0);
			goto finish;
		case ZBX_EVAL_TOKEN_OP_NE:
			value = (0 == eval_variant_compare(left, right) ? 0 : 1);
			goto finish;
	}

	/* check arithmetic operators */

	if (SUCCEED != zbx_variant_convert(left, ZBX_VARIANT_DBL))
	{
		*error = zbx_dsprintf(*error, "left operand \"%s\" is not a numeric value for operator at \"%s\"",
				zbx_variant_value_desc(left), ctx->expression + token->loc.l);
		return FAIL;
	}

	if (SUCCEED != zbx_variant_convert(right, ZBX_VARIANT_DBL))
	{
		*error = zbx_dsprintf(*error, "right operand \"%s\" is not a numeric value for operator at \"%s\"",
				zbx_variant_value_desc(right), ctx->expression + token->loc.l);
		return FAIL;
	}

	/* check logical operators */

	switch (token->type)
	{
		case ZBX_EVAL_TOKEN_OP_AND:
			if (SUCCEED == zbx_double_compare(left->data.dbl, 0) ||
					SUCCEED == zbx_double_compare(right->data.dbl, 0))
			{
				value = 0;
			}
			else
				value = 1;
			goto finish;
		case ZBX_EVAL_TOKEN_OP_OR:
			if (SUCCEED != zbx_double_compare(left->data.dbl, 0) ||
					SUCCEED != zbx_double_compare(right->data.dbl, 0))
			{
				value = 1;
			}
			else
				value = 0;
			goto finish;
	}

	/* check arithmetic operators */

	switch (token->type)
	{
		case ZBX_EVAL_TOKEN_OP_LT:
			value = (0 > zbx_variant_compare(left, right) ? 1 : 0);
			break;
		case ZBX_EVAL_TOKEN_OP_LE:
			value = (0 >= zbx_variant_compare(left, right) ? 1 : 0);
			break;
		case ZBX_EVAL_TOKEN_OP_GT:
			value = (0 < zbx_variant_compare(left, right) ? 1 : 0);
			break;
		case ZBX_EVAL_TOKEN_OP_GE:
			value = (0 <= zbx_variant_compare(left, right) ? 1 : 0);
			break;
		case ZBX_EVAL_TOKEN_OP_ADD:
			value = left->data.dbl + right->data.dbl;
			break;
		case ZBX_EVAL_TOKEN_OP_SUB:
			value = left->data.dbl - right->data.dbl;
			break;
		case ZBX_EVAL_TOKEN_OP_MUL:
			value = left->data.dbl * right->data.dbl;
			break;
		case ZBX_EVAL_TOKEN_OP_DIV:
			if (SUCCEED == zbx_double_compare(right->data.dbl, 0))
			{
				*error = zbx_dsprintf(*error, "division by zero at \"%s\"",
						ctx->expression + token->loc.l);
				return FAIL;
			}
			value = left->data.dbl / right->data.dbl;
			break;
	}
finish:
	zbx_variant_clear(left);
	zbx_variant_clear(right);
	zbx_variant_set_dbl(left, value);
	output->values_num--;

	return SUCCEED;
}

/******************************************************************************
 *                                                                            *
 * Function: eval_suffixed_number_parse                                       *
 *                                                                            *
 * Purpose: check if the value is suffixed number and return the suffix if    *
 *          exists                                                            *
 *                                                                            *
 * Parameters: value  - [IN] the value to check                               *
 *             suffix - [OUT] the suffix or 0 if number does not have suffix  *
 *                            (optional)                                      *
 *                                                                            *
 * Return value: SUCCEED - the value is suffixed number                       *
 *               FAIL    - otherwise                                          *
 *                                                                            *
 ******************************************************************************/
int	 eval_suffixed_number_parse(const char *value, char *suffix)
{
	int	len, num_len;

	if ('-' == *value)
		value++;

	len = strlen(value);

	if (SUCCEED != zbx_suffixed_number_parse(value, &num_len) || num_len != len)
		return FAIL;

	if (NULL != suffix)
		*suffix = value[len - 1];

	return SUCCEED;
}

/******************************************************************************
 *                                                                            *
 * Function: eval_execute_push_value                                          *
 *                                                                            *
 * Purpose: push value in output stack                                        *
 *                                                                            *
 * Parameters: ctx      - [IN] the evaluation context                         *
 *             token    - [IN] the value token                                *
 *             output   - [IN/OUT] the output value stack                     *
 *             error    - [OUT] the error message in the case of failure      *
 *                                                                            *
 * Return value: SUCCEED - the value was pushed successfully                  *
 *               FAIL    - otherwise                                          *
 *                                                                            *
 ******************************************************************************/
static int	eval_execute_push_value(const zbx_eval_context_t *ctx, const zbx_eval_token_t *token,
		zbx_vector_var_t *output, char **error)
{
	zbx_variant_t	value;
	char		*dst;
	const char	*src;

	if (ZBX_VARIANT_NONE == token->value.type)
	{
		if (ZBX_EVAL_TOKEN_VAR_NUM == token->type)
		{
			zbx_uint64_t	ui64;

			if (SUCCEED == is_uint64_n(ctx->expression + token->loc.l, token->loc.r - token->loc.l + 1,
					&ui64))
			{
				zbx_variant_set_ui64(&value, ui64);
			}
			else
			{
				zbx_variant_set_dbl(&value, atof(ctx->expression + token->loc.l) *
						suffix2factor(ctx->expression[token->loc.r]));
			}
		}
		else
		{
			dst = zbx_malloc(NULL, token->loc.r - token->loc.l + 2);
			zbx_variant_set_str(&value, dst);

			if (ZBX_EVAL_TOKEN_VAR_STR == token->type)
			{
				for (src = ctx->expression + token->loc.l + 1; src < ctx->expression + token->loc.r;
						src++)
				{
					if ('\\' == *src)
						src++;
					*dst++ = *src;
				}
			}
			else
			{
				memcpy(dst, ctx->expression + token->loc.l, token->loc.r - token->loc.l + 1);
				dst += token->loc.r - token->loc.l + 1;
			}

			*dst = '\0';
		}
	}
	else
	{
		if (ZBX_VARIANT_ERR == token->value.type && 0 == (ctx->rules & ZBX_EVAL_PROCESS_ERROR))
		{
			*error = zbx_strdup(*error, token->value.data.err);
			return FAIL;
		}

		/* Expanded user macro token variables can contain suffixed numbers. */
		/* Try to convert them and just copy the expanded value if failed.   */
		if (ZBX_EVAL_TOKEN_VAR_USERMACRO != token->type ||
				SUCCEED != variant_convert_suffixed_num(&value, &token->value))
		{
			zbx_variant_copy(&value, &token->value);
		}

	}

	zbx_vector_var_append_ptr(output, &value);

	return SUCCEED;
}

/******************************************************************************
 *                                                                            *
 * Function: eval_execute_push_null                                           *
 *                                                                            *
 * Purpose: push null value in output stack                                   *
 *                                                                            *
 * Parameters: output   - [IN/OUT] the output value stack                     *
 *                                                                            *
 ******************************************************************************/
static void	eval_execute_push_null(zbx_vector_var_t *output)
{
	zbx_variant_t	value;

	zbx_variant_set_none(&value);
	zbx_vector_var_append_ptr(output, &value);
}

/******************************************************************************
 *                                                                            *
 * Function: eval_compare_token                                               *
 *                                                                            *
 * Purpose: check if expression fragment matches the specified text           *
 *                                                                            *
 * Parameters: ctx  - [IN] the evaluation context                             *
 *             loc  - [IN] the expression fragment location                   *
 *             text - [IN] the text to compare with                           *
 *             len  - [IN] the text length                                    *
 *                                                                            *
 * Return value: SUCCEED - the expression fragment matches the text           *
 *               FAIL    - otherwise                                          *
 *                                                                            *
 ******************************************************************************/
int	eval_compare_token(const zbx_eval_context_t *ctx, const zbx_strloc_t *loc, const char *text,
		size_t len)
{
	if (loc->r - loc->l + 1 != len)
		return FAIL;

	if (0 != memcmp(ctx->expression + loc->l, text, len))
		return FAIL;

	return SUCCEED;
}

/******************************************************************************
 *                                                                            *
 * Function: eval_function_return                                             *
 *                                                                            *
 * Purpose: handle function return                                            *
 *                                                                            *
 * Parameters: args_num - [IN] the number of function arguments               *
 *             value    - [IN] the return value                               *
 *             output   - [IN/OUT] the output value stack                     *
 *                                                                            *
 * Comments: The function arguments on output stack are replaced with the     *
 *           return value.                                                    *
 *                                                                            *
 ******************************************************************************/
static void	eval_function_return(int args_num, zbx_variant_t *value, zbx_vector_var_t *output)
{
	int	i;

	for (i = output->values_num - args_num; i < output->values_num; i++)
		zbx_variant_clear(&output->values[i]);
	output->values_num -= args_num;

	zbx_vector_var_append_ptr(output, value);
}

/******************************************************************************
 *                                                                            *
 * Function: eval_validate_function_args                                      *
 *                                                                            *
 * Purpose: validate function arguments                                       *
 *                                                                            *
 * Parameters: ctx    - [IN] the evaluation context                           *
 *             token  - [IN] the function token                               *
 *             output - [IN/OUT] the output value stack                       *
 *             error  - [OUT] the error message in the case of failure        *
 *                                                                            *
 * Return value: SUCCEED - function arguments contain error values - the      *
 *                         first error is returned as function value without  *
 *                         evaluating the function                            *
 *               FAIL    - argument validation failed                         *
 *               UNKNOWN - argument validation succeeded, function result is  *
 *                         unknown at the moment, function must be evaluated  *
 *                         with the prepared arguments                        *
 *                                                                            *
 ******************************************************************************/
static int	eval_validate_function_args(const zbx_eval_context_t *ctx, const zbx_eval_token_t *token,
		zbx_vector_var_t *output, char **error)
{
	int	i;

	if (output->values_num < (int)token->opt)
	{
		*error = zbx_dsprintf(*error, "not enough arguments for function at \"%s\"",
				ctx->expression + token->loc.l);
		return FAIL;
	}

	for (i = output->values_num - token->opt; i < output->values_num; i++)
	{
		if (ZBX_VARIANT_ERR == output->values[i].type)
		{
			zbx_variant_t	value = output->values[i];

			/* first error argument is used as function return value */
			zbx_variant_set_none(&output->values[i]);
			eval_function_return(token->opt, &value, output);

			return SUCCEED;
		}
	}

	return UNKNOWN;
}

static const char	*eval_type_desc(unsigned char type)
{
	switch (type)
	{
		case ZBX_VARIANT_DBL:
			return "a numeric";
		case ZBX_VARIANT_UI64:
			return "an unsigned integer";
		case ZBX_VARIANT_STR:
			return "a string";
		default:
			return zbx_get_variant_type_desc(type);
	}
}

/******************************************************************************
 *                                                                            *
 * Function: eval_convert_function_arg                                        *
 *                                                                            *
 * Purpose: convert function argument to the specified type                   *
 *                                                                            *
 * Parameters: ctx    - [IN] the evaluation context                           *
 *             token  - [IN] the function token                               *
 *             type   - [IN] the required type                                *
 *             arg    - [IN/OUT] the argument to convert                      *
 *             error  - [OUT] the error message in the case of failure        *
 *                                                                            *
 * Return value: SUCCEED - argument was converted successfully                *
 *               FAIL    - otherwise                                          *
 *                                                                            *
 ******************************************************************************/
static int	eval_convert_function_arg(const zbx_eval_context_t *ctx, const zbx_eval_token_t *token,
		unsigned char type, zbx_variant_t *arg, char **error)
{
	zbx_variant_t	value;

	if (ZBX_VARIANT_DBL == type && SUCCEED == variant_convert_suffixed_num(&value, arg))
	{
		zbx_variant_clear(arg);
		*arg = value;
		return SUCCEED;
	}

	if (SUCCEED == zbx_variant_convert(arg, type))
		return SUCCEED;

	*error = zbx_dsprintf(*error, "function argument \"%s\" is not %s value at \"%s\"",
			zbx_variant_value_desc(arg),  eval_type_desc(type), ctx->expression + token->loc.l);

	return FAIL;
}

/******************************************************************************
 *                                                                            *
 * Function: eval_prepare_math_function_args                                  *
 *                                                                            *
 * Purpose: validate and prepare (convert to floating values) math function   *
 *          arguments                                                         *
 *                                                                            *
 * Parameters: ctx    - [IN] the evaluation context                           *
 *             token  - [IN] the function token                               *
 *             output - [IN/OUT] the output value stack                       *
 *             error  - [OUT] the error message in the case of failure        *
 *                                                                            *
 * Return value: SUCCEED - function arguments contain error values - the      *
 *                         first error is returned as function value without  *
 *                         evaluating the function                            *
 *               FAIL    - argument validation/conversion failed              *
 *               UNKNOWN - argument conversion succeeded, function result is  *
 *                         unknown at the moment, function must be evaluated  *
 *                         with the prepared arguments                        *
 *                                                                            *
 * Comments: Math function accepts either 1+ arguments that can be converted  *
 *           to floating values or a single argument of non-zero length       *
 *           floating value vector.                                           *
 *                                                                            *
 ******************************************************************************/
static int	eval_prepare_math_function_args(const zbx_eval_context_t *ctx, const zbx_eval_token_t *token,
		zbx_vector_var_t *output, char **error)
{
	int	i, ret;

	if (UNKNOWN != (ret = eval_validate_function_args(ctx, token, output, error)))
		return ret;

	i = output->values_num - token->opt;

	if (ZBX_VARIANT_DBL_VECTOR != output->values[i].type)
	{
		for (; i < output->values_num; i++)
		{
			if (SUCCEED != eval_convert_function_arg(ctx, token, ZBX_VARIANT_DBL, &output->values[i], error))
				return FAIL;
		}
	}
	else
	{
		if (1 != token->opt)
		{
			*error = zbx_dsprintf(*error, "too many arguments for function at \"%s\"",
					ctx->expression + token->loc.l);
			return FAIL;
		}

		if (0 == output->values[i].data.dbl_vector->values_num)
		{
			*error = zbx_dsprintf(*error, "empty vector argument for function at \"%s\"",
					ctx->expression + token->loc.l);
			return FAIL;
		}
	}

	return UNKNOWN;
}

/******************************************************************************
 *                                                                            *
 * Function: eval_execute_function_min                                        *
 *                                                                            *
 * Purpose: evaluate min() function                                           *
 *                                                                            *
 * Parameters: ctx    - [IN] the evaluation context                           *
 *             token  - [IN] the function token                               *
 *             output - [IN/OUT] the output value stack                       *
 *             error  - [OUT] the error message in the case of failure        *
 *                                                                            *
 * Return value: SUCCEED - function evaluation succeeded                      *
 *               FAIL    - otherwise                                          *
 *                                                                            *
 ******************************************************************************/
static int	eval_execute_function_min(const zbx_eval_context_t *ctx, const zbx_eval_token_t *token,
		zbx_vector_var_t *output, char **error)
{
	int		i, ret;
	double		min;
	zbx_variant_t	value;

	if (UNKNOWN != (ret = eval_prepare_math_function_args(ctx, token, output, error)))
		return ret;

	i = output->values_num - token->opt;

	if (ZBX_VARIANT_DBL_VECTOR != output->values[i].type)
	{
		min = output->values[i++].data.dbl;

		for (; i < output->values_num; i++)
		{
			if (min > output->values[i].data.dbl)
				min = output->values[i].data.dbl;
		}
	}
	else
	{
		zbx_vector_dbl_t	*dbl_vector = output->values[i].data.dbl_vector;

		min = dbl_vector->values[0];

		for (i = 1; i < dbl_vector->values_num; i++)
		{
			if (min > dbl_vector->values[i])
				min = dbl_vector->values[i];
		}
	}

	zbx_variant_set_dbl(&value, min);
	eval_function_return(token->opt, &value, output);

	return SUCCEED;
}

/******************************************************************************
 *                                                                            *
 * Function: eval_execute_function_max                                        *
 *                                                                            *
 * Purpose: evaluate max() function                                           *
 *                                                                            *
 * Parameters: ctx    - [IN] the evaluation context                           *
 *             token  - [IN] the function token                               *
 *             output - [IN/OUT] the output value stack                       *
 *             error  - [OUT] the error message in the case of failure        *
 *                                                                            *
 * Return value: SUCCEED - function evaluation succeeded                      *
 *               FAIL    - otherwise                                          *
 *                                                                            *
 ******************************************************************************/
static int	eval_execute_function_max(const zbx_eval_context_t *ctx, const zbx_eval_token_t *token,
		zbx_vector_var_t *output, char **error)
{
	int		i, ret;
	double		max;
	zbx_variant_t	value;

	if (UNKNOWN != (ret = eval_prepare_math_function_args(ctx, token, output, error)))
		return ret;

	i = output->values_num - token->opt;

	if (ZBX_VARIANT_DBL_VECTOR != output->values[i].type)
	{
		max = output->values[i++].data.dbl;

		for (; i < output->values_num; i++)
		{
			if (max < output->values[i].data.dbl)
				max = output->values[i].data.dbl;
		}
	}
	else
	{
		zbx_vector_dbl_t	*dbl_vector = output->values[i].data.dbl_vector;

		max = dbl_vector->values[0];

		for (i = 1; i < dbl_vector->values_num; i++)
		{
			if (max < dbl_vector->values[i])
				max = dbl_vector->values[i];
		}
	}

	zbx_variant_set_dbl(&value, max);
	eval_function_return(token->opt, &value, output);

	return SUCCEED;
}

/******************************************************************************
 *                                                                            *
 * Function: eval_execute_function_sum                                        *
 *                                                                            *
 * Purpose: evaluate sum() function                                           *
 *                                                                            *
 * Parameters: ctx    - [IN] the evaluation context                           *
 *             token  - [IN] the function token                               *
 *             output - [IN/OUT] the output value stack                       *
 *             error  - [OUT] the error message in the case of failure        *
 *                                                                            *
 * Return value: SUCCEED - function evaluation succeeded                      *
 *               FAIL    - otherwise                                          *
 *                                                                            *
 ******************************************************************************/
static int	eval_execute_function_sum(const zbx_eval_context_t *ctx, const zbx_eval_token_t *token,
		zbx_vector_var_t *output, char **error)
{
	int		i, ret;
	double		sum = 0;
	zbx_variant_t	value;

	if (UNKNOWN != (ret = eval_prepare_math_function_args(ctx, token, output, error)))
		return ret;

	i = output->values_num - token->opt;

	if (ZBX_VARIANT_DBL_VECTOR != output->values[i].type)
	{
		for (; i < output->values_num; i++)
			sum += output->values[i].data.dbl;
	}
	else
	{
		zbx_vector_dbl_t	*dbl_vector = output->values[i].data.dbl_vector;

		for (i = 0; i < dbl_vector->values_num; i++)
			sum += dbl_vector->values[i];
	}

	zbx_variant_set_dbl(&value, sum);
	eval_function_return(token->opt, &value, output);

	return SUCCEED;
}

/******************************************************************************
 *                                                                            *
 * Function: eval_execute_function_avg                                        *
 *                                                                            *
 * Purpose: evaluate avg() function                                           *
 *                                                                            *
 * Parameters: ctx    - [IN] the evaluation context                           *
 *             token  - [IN] the function token                               *
 *             output - [IN/OUT] the output value stack                       *
 *             error  - [OUT] the error message in the case of failure        *
 *                                                                            *
 * Return value: SUCCEED - function evaluation succeeded                      *
 *               FAIL    - otherwise                                          *
 *                                                                            *
 ******************************************************************************/
static int	eval_execute_function_avg(const zbx_eval_context_t *ctx, const zbx_eval_token_t *token,
		zbx_vector_var_t *output, char **error)
{
	int		i, ret;
	double		avg = 0;
	zbx_variant_t	value;

	if (UNKNOWN != (ret = eval_prepare_math_function_args(ctx, token, output, error)))
		return ret;

	i = output->values_num - token->opt;

	if (ZBX_VARIANT_DBL_VECTOR != output->values[i].type)
	{
		for (; i < output->values_num; i++)
			avg += output->values[i].data.dbl;

		avg /= token->opt;
	}
	else
	{
		zbx_vector_dbl_t	*dbl_vector = output->values[i].data.dbl_vector;

		for (i = 0; i < dbl_vector->values_num; i++)
			avg += dbl_vector->values[i];

		avg /= dbl_vector->values_num;
	}

	zbx_variant_set_dbl(&value, avg);
	eval_function_return(token->opt, &value, output);

	return SUCCEED;
}

/******************************************************************************
 *                                                                            *
 * Function: eval_execute_function_abs                                     *
 *                                                                            *
 * Purpose: evaluate abs() function                                        *
 *                                                                            *
 * Parameters: ctx    - [IN] the evaluation context                           *
 *             token  - [IN] the function token                               *
 *             output - [IN/OUT] the output value stack                       *
 *             error  - [OUT] the error message in the case of failure        *
 *                                                                            *
 * Return value: SUCCEED - function evaluation succeeded                      *
 *               FAIL    - otherwise                                          *
 *                                                                            *
 ******************************************************************************/
static int	eval_execute_function_abs(const zbx_eval_context_t *ctx, const zbx_eval_token_t *token,
		zbx_vector_var_t *output, char **error)
{
	int		ret;
	zbx_variant_t	*arg, value;

	if (1 != token->opt)
	{
		*error = zbx_dsprintf(*error, "invalid number of arguments for function at \"%s\"",
				ctx->expression + token->loc.l);
		return FAIL;
	}

	if (UNKNOWN != (ret = eval_prepare_math_function_args(ctx, token, output, error)))
		return ret;

	arg = &output->values[output->values_num - 1];
	zbx_variant_set_dbl(&value, fabs(arg->data.dbl));
	eval_function_return(token->opt, &value, output);

	return SUCCEED;
}

/******************************************************************************
 *                                                                            *
 * Function: eval_execute_function_length                                     *
 *                                                                            *
 * Purpose: evaluate length() function                                        *
 *                                                                            *
 * Parameters: ctx    - [IN] the evaluation context                           *
 *             token  - [IN] the function token                               *
 *             output - [IN/OUT] the output value stack                       *
 *             error  - [OUT] the error message in the case of failure        *
 *                                                                            *
 * Return value: SUCCEED - function evaluation succeeded                      *
 *               FAIL    - otherwise                                          *
 *                                                                            *
 ******************************************************************************/
static int	eval_execute_function_length(const zbx_eval_context_t *ctx, const zbx_eval_token_t *token,
		zbx_vector_var_t *output, char **error)
{
	int		ret;
	zbx_variant_t	*arg, value;

	if (1 != token->opt)
	{
		*error = zbx_dsprintf(*error, "invalid number of arguments for function at \"%s\"",
				ctx->expression + token->loc.l);
		return FAIL;
	}

	if (UNKNOWN != (ret = eval_validate_function_args(ctx, token, output, error)))
		return ret;

	arg = &output->values[output->values_num - 1];

	if (SUCCEED != eval_convert_function_arg(ctx, token, ZBX_VARIANT_STR, arg, error))
		return FAIL;

	zbx_variant_set_dbl(&value, zbx_strlen_utf8(arg->data.str));
	eval_function_return(1, &value, output);

	return SUCCEED;
}

/******************************************************************************
 *                                                                            *
 * Function: eval_execute_function_date                                       *
 *                                                                            *
 * Purpose: evaluate date() function                                          *
 *                                                                            *
 * Parameters: ctx    - [IN] the evaluation context                           *
 *             token  - [IN] the function token                               *
 *             output - [IN/OUT] the output value stack                       *
 *             error  - [OUT] the error message in the case of failure        *
 *                                                                            *
 * Return value: SUCCEED - function evaluation succeeded                      *
 *               FAIL    - otherwise                                          *
 *                                                                            *
 ******************************************************************************/
static int	eval_execute_function_date(const zbx_eval_context_t *ctx, const zbx_eval_token_t *token,
		zbx_vector_var_t *output, char **error)
{
	zbx_variant_t	value;
	struct tm	*tm;
	time_t		now;

	if (0 != token->opt)
	{
		*error = zbx_dsprintf(*error, "invalid number of arguments for function at \"%s\"",
				ctx->expression + token->loc.l);
		return FAIL;
	}

	now = ctx->ts.sec;
	if (NULL == (tm = localtime(&now)))
	{
		*error = zbx_dsprintf(*error, "cannot convert time for function at \"%s\": %s",
				ctx->expression + token->loc.l, zbx_strerror(errno));
		return FAIL;
	}
	zbx_variant_set_str(&value, zbx_dsprintf(NULL, "%.4d%.2d%.2d", tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday));
	eval_function_return(0, &value, output);

	return SUCCEED;
}

/******************************************************************************
 *                                                                            *
 * Function: eval_execute_function_time                                       *
 *                                                                            *
 * Purpose: evaluate time() function                                          *
 *                                                                            *
 * Parameters: ctx    - [IN] the evaluation context                           *
 *             token  - [IN] the function token                               *
 *             output - [IN/OUT] the output value stack                       *
 *             error  - [OUT] the error message in the case of failure        *
 *                                                                            *
 * Return value: SUCCEED - function evaluation succeeded                      *
 *               FAIL    - otherwise                                          *
 *                                                                            *
 ******************************************************************************/
static int	eval_execute_function_time(const zbx_eval_context_t *ctx, const zbx_eval_token_t *token,
		zbx_vector_var_t *output, char **error)
{
	zbx_variant_t	value;
	struct tm	*tm;
	time_t		now;

	if (0 != token->opt)
	{
		*error = zbx_dsprintf(*error, "invalid number of arguments for function at \"%s\"",
				ctx->expression + token->loc.l);
		return FAIL;
	}

	now = ctx->ts.sec;
	if (NULL == (tm = localtime(&now)))
	{
		*error = zbx_dsprintf(*error, "cannot convert time for function at \"%s\": %s",
				ctx->expression + token->loc.l, zbx_strerror(errno));
		return FAIL;
	}
	zbx_variant_set_str(&value, zbx_dsprintf(NULL, "%.2d%.2d%.2d", tm->tm_hour, tm->tm_min, tm->tm_sec));
	eval_function_return(0, &value, output);

	return SUCCEED;
}
/******************************************************************************
 *                                                                            *
 * Function: eval_execute_function_now                                        *
 *                                                                            *
 * Purpose: evaluate now() function                                           *
 *                                                                            *
 * Parameters: ctx    - [IN] the evaluation context                           *
 *             token  - [IN] the function token                               *
 *             output - [IN/OUT] the output value stack                       *
 *             error  - [OUT] the error message in the case of failure        *
 *                                                                            *
 * Return value: SUCCEED - function evaluation succeeded                      *
 *               FAIL    - otherwise                                          *
 *                                                                            *
 ******************************************************************************/
static int	eval_execute_function_now(const zbx_eval_context_t *ctx, const zbx_eval_token_t *token,
		zbx_vector_var_t *output, char **error)
{
	zbx_variant_t	value;

	if (0 != token->opt)
	{
		*error = zbx_dsprintf(*error, "invalid number of arguments for function at \"%s\"",
				ctx->expression + token->loc.l);
		return FAIL;
	}

	zbx_variant_set_str(&value, zbx_dsprintf(NULL, "%d", ctx->ts.sec));
	eval_function_return(0, &value, output);

	return SUCCEED;
}
/******************************************************************************
 *                                                                            *
 * Function: eval_execute_function_dayofweek                                  *
 *                                                                            *
 * Purpose: evaluate dayofweek() function                                     *
 *                                                                            *
 * Parameters: ctx    - [IN] the evaluation context                           *
 *             token  - [IN] the function token                               *
 *             output - [IN/OUT] the output value stack                       *
 *             error  - [OUT] the error message in the case of failure        *
 *                                                                            *
 * Return value: SUCCEED - function evaluation succeeded                      *
 *               FAIL    - otherwise                                          *
 *                                                                            *
 ******************************************************************************/
static int	eval_execute_function_dayofweek(const zbx_eval_context_t *ctx, const zbx_eval_token_t *token,
		zbx_vector_var_t *output, char **error)
{
	zbx_variant_t	value;
	struct tm	*tm;
	time_t		now;

	if (0 != token->opt)
	{
		*error = zbx_dsprintf(*error, "invalid number of arguments for function at \"%s\"",
				ctx->expression + token->loc.l);
		return FAIL;
	}

	now = ctx->ts.sec;
	if (NULL == (tm = localtime(&now)))
	{
		*error = zbx_dsprintf(*error, "cannot convert time for function at \"%s\": %s",
				ctx->expression + token->loc.l, zbx_strerror(errno));
		return FAIL;
	}
	zbx_variant_set_str(&value, zbx_dsprintf(NULL, "%d", 0 == tm->tm_wday ? 7 : tm->tm_wday));
	eval_function_return(0, &value, output);

	return SUCCEED;
}

/******************************************************************************
 *                                                                            *
 * Function: eval_execute_function_dayofmonth                                 *
 *                                                                            *
 * Purpose: evaluate dayofmonth() function                                    *
 *                                                                            *
 * Parameters: ctx    - [IN] the evaluation context                           *
 *             token  - [IN] the function token                               *
 *             output - [IN/OUT] the output value stack                       *
 *             error  - [OUT] the error message in the case of failure        *
 *                                                                            *
 * Return value: SUCCEED - function evaluation succeeded                      *
 *               FAIL    - otherwise                                          *
 *                                                                            *
 ******************************************************************************/
static int	eval_execute_function_dayofmonth(const zbx_eval_context_t *ctx, const zbx_eval_token_t *token,
		zbx_vector_var_t *output, char **error)
{
	zbx_variant_t	value;
	struct tm	*tm;
	time_t		now;

	if (0 != token->opt)
	{
		*error = zbx_dsprintf(*error, "invalid number of arguments for function at \"%s\"",
				ctx->expression + token->loc.l);
		return FAIL;
	}

	now = ctx->ts.sec;
	if (NULL == (tm = localtime(&now)))
	{
		*error = zbx_dsprintf(*error, "cannot convert time for function at \"%s\": %s",
				ctx->expression + token->loc.l, zbx_strerror(errno));
		return FAIL;
	}
	zbx_variant_set_str(&value, zbx_dsprintf(NULL, "%d", tm->tm_mday));
	eval_function_return(0, &value, output);

	return SUCCEED;
}

/******************************************************************************
 *                                                                            *
 * Function: eval_execute_function_bitwise                                    *
 *                                                                            *
 * Purpose: evaluate bitand(), bitor(), bitxor(), bitlshift(),                *
 *          bitrshift() functions                                             *
 *                                                                            *
 * Parameters: ctx    - [IN] the evaluation context                           *
 *             token  - [IN] the function token                               *
 *             type   - [IN] the function type                                *
 *             output - [IN/OUT] the output value stack                       *
 *             error  - [OUT] the error message in the case of failure        *
 *                                                                            *
 * Return value: SUCCEED - function evaluation succeeded                      *
 *               FAIL    - otherwise                                          *
 *                                                                            *
 ******************************************************************************/
static int	eval_execute_function_bitwise(const zbx_eval_context_t *ctx, const zbx_eval_token_t *token,
		zbx_function_bit_optype_t type, zbx_vector_var_t *output, char **error)
{
	zbx_variant_t	value, *left, *right;
	int		ret;

	if (2 != token->opt)
	{
		*error = zbx_dsprintf(*error, "invalid number of arguments for function at \"%s\"",
				ctx->expression + token->loc.l);
		return FAIL;
	}

	if (UNKNOWN != (ret = eval_validate_function_args(ctx, token, output, error)))
		return ret;

	left = &output->values[output->values_num - 2];
	right = &output->values[output->values_num - 1];

	if (SUCCEED != zbx_variant_convert(left, ZBX_VARIANT_UI64))
	{
		*error = zbx_dsprintf(*error, "function argument \"%s\" is not an unsigned integer value at \"%s\"",
				zbx_variant_value_desc(left), ctx->expression + token->loc.l);
		return FAIL;
	}

	if (SUCCEED != zbx_variant_convert(right, ZBX_VARIANT_UI64))
	{
		*error = zbx_dsprintf(*error, "function argument \"%s\" is not an unsigned integer value at \"%s\"",
				zbx_variant_value_desc(right), ctx->expression + token->loc.l);
		return FAIL;
	}

	switch (type)
	{
		case FUNCTION_OPTYPE_BIT_AND:
			zbx_variant_set_ui64(&value, left->data.ui64 & right->data.ui64);
			break;
		case FUNCTION_OPTYPE_BIT_OR:
			zbx_variant_set_ui64(&value, left->data.ui64 | right->data.ui64);
			break;
		case FUNCTION_OPTYPE_BIT_XOR:
			zbx_variant_set_ui64(&value, left->data.ui64 ^ right->data.ui64);
			break;
		case FUNCTION_OPTYPE_BIT_LSHIFT:
			zbx_variant_set_ui64(&value, left->data.ui64 << right->data.ui64);
			break;
		case FUNCTION_OPTYPE_BIT_RSHIFT:
			zbx_variant_set_ui64(&value, left->data.ui64 >> right->data.ui64);
	}

	eval_function_return(2, &value, output);

	return SUCCEED;
}

/******************************************************************************
 *                                                                            *
 * Function: eval_execute_function_bitnot                                     *
 *                                                                            *
 * Purpose: evaluate bitnot() function                                        *
 *                                                                            *
 * Parameters: ctx    - [IN] the evaluation context                           *
 *             token  - [IN] the function token                               *
 *             output - [IN/OUT] the output value stack                       *
 *             error  - [OUT] the error message in the case of failure        *
 *                                                                            *
 * Return value: SUCCEED - function evaluation succeeded                      *
 *               FAIL    - otherwise                                          *
 *                                                                            *
 ******************************************************************************/
static int	eval_execute_function_bitnot(const zbx_eval_context_t *ctx, const zbx_eval_token_t *token,
		zbx_vector_var_t *output, char **error)
{
	zbx_variant_t	value, *arg;
	int		ret;

	if (1 != token->opt)
	{
		*error = zbx_dsprintf(*error, "invalid number of arguments for function at \"%s\"",
				ctx->expression + token->loc.l);
		return FAIL;
	}

	if (UNKNOWN != (ret = eval_validate_function_args(ctx, token, output, error)))
		return ret;

	arg = &output->values[output->values_num - 1];

	if (SUCCEED != zbx_variant_convert(arg, ZBX_VARIANT_UI64))
	{
		*error = zbx_dsprintf(*error, "function argument \"%s\" is not an unsigned integer value at \"%s\"",
				zbx_variant_value_desc(arg), ctx->expression + token->loc.l);
		return FAIL;
	}

	zbx_variant_set_ui64(&value, ~arg->data.ui64);
	eval_function_return(1, &value, output);

	return SUCCEED;
}

/******************************************************************************
 *                                                                            *
 * Function: eval_execute_function_left                                       *
 *                                                                            *
 * Purpose: evaluate left() function                                          *
 *                                                                            *
 * Parameters: ctx    - [IN] the evaluation context                           *
 *             token  - [IN] the function token                               *
 *             output - [IN/OUT] the output value stack                       *
 *             error  - [OUT] the error message in the case of failure        *
 *                                                                            *
 * Return value: SUCCEED - function evaluation succeeded                      *
 *               FAIL    - otherwise                                          *
 *                                                                            *
 ******************************************************************************/
static int	eval_execute_function_left(const zbx_eval_context_t *ctx, const zbx_eval_token_t *token,
		zbx_vector_var_t *output, char **error)
{
	int		ret;
	zbx_variant_t	*arg, *len, value;
	size_t		sz;
	char		*strval;

	if (2 != token->opt)
	{
		*error = zbx_dsprintf(*error, "invalid number of arguments for function at \"%s\"",
				ctx->expression + token->loc.l);
		return FAIL;
	}

	if (UNKNOWN != (ret = eval_validate_function_args(ctx, token, output, error)))
		return ret;

	arg = &output->values[output->values_num - 2];
	len = &output->values[output->values_num - 1];

	if (SUCCEED != zbx_variant_convert(arg, ZBX_VARIANT_STR))
	{
		*error = zbx_dsprintf(*error, "invalid function first argument at \"%s\"",
				ctx->expression + token->loc.l);
		return FAIL;
	}

	if (SUCCEED != zbx_variant_convert(len, ZBX_VARIANT_UI64))
	{
		*error = zbx_dsprintf(*error, "function argument \"%s\" is not an unsigned integer value at \"%s\"",
				zbx_variant_value_desc(len), ctx->expression + token->loc.l);
		return FAIL;
	}

	sz = zbx_strlen_utf8_nchars(arg->data.str, (size_t)len->data.ui64) + 1;
	strval = zbx_malloc(NULL, sz);
	zbx_strlcpy_utf8(strval, arg->data.str, sz);

	zbx_variant_set_str(&value, strval);
	eval_function_return(2, &value, output);

	return SUCCEED;
}

/******************************************************************************
 *                                                                            *
 * Function: eval_execute_function_right                                      *
 *                                                                            *
 * Purpose: evaluate right() function                                         *
 *                                                                            *
 * Parameters: ctx    - [IN] the evaluation context                           *
 *             token  - [IN] the function token                               *
 *             output - [IN/OUT] the output value stack                       *
 *             error  - [OUT] the error message in the case of failure        *
 *                                                                            *
 * Return value: SUCCEED - function evaluation succeeded                      *
 *               FAIL    - otherwise                                          *
 *                                                                            *
 ******************************************************************************/
static int	eval_execute_function_right(const zbx_eval_context_t *ctx, const zbx_eval_token_t *token,
		zbx_vector_var_t *output, char **error)
{
	int		ret;
	zbx_variant_t	*arg, *len, value;
	size_t		sz, srclen;
	char		*strval, *p;

	if (2 != token->opt)
	{
		*error = zbx_dsprintf(*error, "invalid number of arguments for function at \"%s\"",
				ctx->expression + token->loc.l);
		return FAIL;
	}

	if (UNKNOWN != (ret = eval_validate_function_args(ctx, token, output, error)))
		return ret;

	arg = &output->values[output->values_num - 2];
	len = &output->values[output->values_num - 1];

	if (SUCCEED != zbx_variant_convert(arg, ZBX_VARIANT_STR))
	{
		*error = zbx_dsprintf(*error, "invalid function first argument at \"%s\"",
				ctx->expression + token->loc.l);
		return FAIL;
	}

	if (SUCCEED != zbx_variant_convert(len, ZBX_VARIANT_UI64))
	{
		*error = zbx_dsprintf(*error, "function argument \"%s\" is not an unsigned integer value at \"%s\"",
				zbx_variant_value_desc(len), ctx->expression + token->loc.l);
		return FAIL;
	}

	srclen = zbx_strlen_utf8(arg->data.str);

	if (len->data.ui64 < srclen)
	{
		p = zbx_strshift_utf8(arg->data.str, srclen - len->data.ui64);
		sz = zbx_strlen_utf8_nchars(p, (size_t)len->data.ui64) + 1;
		strval = zbx_malloc(NULL, sz);
		zbx_strlcpy_utf8(strval, p, sz);
	}
	else
		strval = zbx_strdup(NULL, arg->data.str);

	zbx_variant_set_str(&value, strval);
	eval_function_return(2, &value, output);

	return SUCCEED;
}

/******************************************************************************
 *                                                                            *
 * Function: eval_execute_function_mid                                        *
 *                                                                            *
 * Purpose: evaluate mid() function                                           *
 *                                                                            *
 * Parameters: ctx    - [IN] the evaluation context                           *
 *             token  - [IN] the function token                               *
 *             output - [IN/OUT] the output value stack                       *
 *             error  - [OUT] the error message in the case of failure        *
 *                                                                            *
 * Return value: SUCCEED - function evaluation succeeded                      *
 *               FAIL    - otherwise                                          *
 *                                                                            *
 ******************************************************************************/
static int	eval_execute_function_mid(const zbx_eval_context_t *ctx, const zbx_eval_token_t *token,
		zbx_vector_var_t *output, char **error)
{
	int		ret;
	zbx_variant_t	*arg, *start, *len, value;
	size_t		sz, srclen;
	char		*strval, *p;

	if (3 != token->opt)
	{
		*error = zbx_dsprintf(*error, "invalid number of arguments for function at \"%s\"",
				ctx->expression + token->loc.l);
		return FAIL;
	}

	if (UNKNOWN != (ret = eval_validate_function_args(ctx, token, output, error)))
		return ret;

	arg = &output->values[output->values_num - 3];
	start = &output->values[output->values_num - 2];
	len = &output->values[output->values_num - 1];

	if (SUCCEED != zbx_variant_convert(arg, ZBX_VARIANT_STR))
	{
		*error = zbx_dsprintf(*error, "invalid function first argument at \"%s\"",
				ctx->expression + token->loc.l);
		return FAIL;
	}

	srclen = zbx_strlen_utf8(arg->data.str);

	if (SUCCEED != zbx_variant_convert(start, ZBX_VARIANT_UI64) || 0 == start->data.ui64 ||
			start->data.ui64 > srclen)
	{
		*error = zbx_dsprintf(*error, "invalid function second argument at \"%s\"",
				ctx->expression + token->loc.l);
		return FAIL;
	}

	if (SUCCEED != zbx_variant_convert(len, ZBX_VARIANT_UI64))
	{
		*error = zbx_dsprintf(*error, "function argument \"%s\" is not an unsigned integer value at \"%s\"",
				zbx_variant_value_desc(len), ctx->expression + token->loc.l);
		return FAIL;
	}

	p = zbx_strshift_utf8(arg->data.str, start->data.ui64 - 1);

	if (srclen > start->data.ui64 + len->data.ui64)
	{
		sz = zbx_strlen_utf8_nchars(p, len->data.ui64) + 1;
		strval = zbx_malloc(NULL, sz);
		zbx_strlcpy_utf8(strval, p, sz);
	}
	else
		strval = zbx_strdup(NULL, p);

	zbx_variant_set_str(&value, strval);
	eval_function_return(3, &value, output);

	return SUCCEED;
}

/******************************************************************************
 *                                                                            *
 * Function: eval_execute_function_trim                                       *
 *                                                                            *
 * Purpose: evaluate trim(), rtrim(), ltrim() functions                       *
 *                                                                            *
 * Parameters: ctx    - [IN] the evaluation context                           *
 *             token  - [IN] the function token                               *
 *             type   - [IN] the function type                                *
 *             output - [IN/OUT] the output value stack                       *
 *             error  - [OUT] the error message in the case of failure        *
 *                                                                            *
 * Return value: SUCCEED - function evaluation succeeded                      *
 *               FAIL    - otherwise                                          *
 *                                                                            *
 ******************************************************************************/
static int	eval_execute_function_trim(const zbx_eval_context_t *ctx, const zbx_eval_token_t *token,
		zbx_function_trim_optype_t type, zbx_vector_var_t *output, char **error)
{
	int		ret;
	zbx_variant_t	*sym, *arg, value, sym_val;

	if (1 > token->opt || 2 < token->opt)
	{
		*error = zbx_dsprintf(*error, "invalid number of arguments for function at \"%s\"",
				ctx->expression + token->loc.l);
		return FAIL;
	}

	if (UNKNOWN != (ret = eval_validate_function_args(ctx, token, output, error)))
		return ret;

	if (2 == token->opt)
	{
		arg = &output->values[output->values_num - 2];
		sym = &output->values[output->values_num - 1];

		if (SUCCEED != zbx_variant_convert(sym, ZBX_VARIANT_STR))
		{
			*error = zbx_dsprintf(*error, "invalid function second argument at \"%s\"",
					ctx->expression + token->loc.l);
			return FAIL;
		}
	}
	else
	{
		arg = &output->values[output->values_num - 1];
		zbx_variant_set_str(&sym_val, zbx_strdup(NULL, ZBX_WHITESPACE));
		sym = &sym_val;
	}

	if (SUCCEED != zbx_variant_convert(arg, ZBX_VARIANT_STR))
	{
		*error = zbx_dsprintf(*error, "invalid function first argument at \"%s\"",
				ctx->expression + token->loc.l);
		return FAIL;
	}

	switch (type)
	{
		case FUNCTION_OPTYPE_TRIM_ALL:
			zbx_lrtrim(arg->data.str, sym->data.str);
			break;
		case FUNCTION_OPTYPE_TRIM_RIGHT:
			zbx_rtrim(arg->data.str, sym->data.str);
			break;
		case FUNCTION_OPTYPE_TRIM_LEFT:
			zbx_ltrim(arg->data.str, sym->data.str);
	}

	if (2 != token->opt)
		zbx_variant_clear(&sym_val);

	zbx_variant_set_str(&value, zbx_strdup(NULL, arg->data.str));
	eval_function_return(token->opt, &value, output);

	return SUCCEED;
}

/******************************************************************************
 *                                                                            *
 * Function: eval_execute_function_concat                                     *
 *                                                                            *
 * Purpose: evaluate concat() function                                        *
 *                                                                            *
 * Parameters: ctx    - [IN] the evaluation context                           *
 *             token  - [IN] the function token                               *
 *             output - [IN/OUT] the output value stack                       *
 *             error  - [OUT] the error message in the case of failure        *
 *                                                                            *
 * Return value: SUCCEED - function evaluation succeeded                      *
 *               FAIL    - otherwise                                          *
 *                                                                            *
 ******************************************************************************/
static int	eval_execute_function_concat(const zbx_eval_context_t *ctx, const zbx_eval_token_t *token,
		zbx_vector_var_t *output, char **error)
{
	int		ret;
	zbx_variant_t	*str1, *str2, value;

	if (2 != token->opt)
	{
		*error = zbx_dsprintf(*error, "invalid number of arguments for function at \"%s\"",
				ctx->expression + token->loc.l);
		return FAIL;
	}

	if (UNKNOWN != (ret = eval_validate_function_args(ctx, token, output, error)))
		return ret;

	str1 = &output->values[output->values_num - 2];
	str2 = &output->values[output->values_num - 1];

	if (SUCCEED != zbx_variant_convert(str1, ZBX_VARIANT_STR))
	{
		*error = zbx_dsprintf(*error, "invalid function first argument at \"%s\"",
				ctx->expression + token->loc.l);
		return FAIL;
	}

	if (SUCCEED != zbx_variant_convert(str2, ZBX_VARIANT_STR))
	{
		*error = zbx_dsprintf(*error, "invalid function second argument at \"%s\"",
				ctx->expression + token->loc.l);
		return FAIL;
	}

	zbx_variant_set_str(&value, zbx_strdcat(str1->data.str, str2->data.str));
	eval_function_return(2, &value, output);

	return SUCCEED;
}

/******************************************************************************
 *                                                                            *
 * Function: eval_execute_function_insert                                     *
 *                                                                            *
 * Purpose: evaluate insert() function                                        *
 *                                                                            *
 * Parameters: ctx    - [IN] the evaluation context                           *
 *             token  - [IN] the function token                               *
 *             output - [IN/OUT] the output value stack                       *
 *             error  - [OUT] the error message in the case of failure        *
 *                                                                            *
 * Return value: SUCCEED - function evaluation succeeded                      *
 *               FAIL    - otherwise                                          *
 *                                                                            *
 ******************************************************************************/
static int	eval_execute_function_insert(const zbx_eval_context_t *ctx, const zbx_eval_token_t *token,
		zbx_vector_var_t *output, char **error)
{
	int		ret;
	zbx_variant_t	*arg, *start, *len, *replacement, value;
	char		*strval;
	size_t		str_alloc, str_len;

	if (4 != token->opt)
	{
		*error = zbx_dsprintf(*error, "invalid number of arguments for function at \"%s\"",
				ctx->expression + token->loc.l);
		return FAIL;
	}

	if (UNKNOWN != (ret = eval_validate_function_args(ctx, token, output, error)))
		return ret;

	arg = &output->values[output->values_num - 4];
	start = &output->values[output->values_num - 3];
	len = &output->values[output->values_num - 2];
	replacement = &output->values[output->values_num - 1];

	if (SUCCEED != zbx_variant_convert(arg, ZBX_VARIANT_STR))
	{
		*error = zbx_dsprintf(*error, "invalid function first argument at \"%s\"",
				ctx->expression + token->loc.l);
		return FAIL;
	}

	if (SUCCEED != zbx_variant_convert(start, ZBX_VARIANT_UI64))
	{
		*error = zbx_dsprintf(*error, "function argument \"%s\" is not an unsigned integer value at \"%s\"",
				zbx_variant_value_desc(start), ctx->expression + token->loc.l);
		return FAIL;
	}

	if (SUCCEED != zbx_variant_convert(len, ZBX_VARIANT_UI64))
	{
		*error = zbx_dsprintf(*error, "function argument \"%s\" is not an unsigned integer value at \"%s\"",
				zbx_variant_value_desc(len), ctx->expression + token->loc.l);
		return FAIL;
	}

	if (SUCCEED != zbx_variant_convert(replacement, ZBX_VARIANT_STR))
	{
		*error = zbx_dsprintf(*error, "invalid function fourth argument at \"%s\"",
				ctx->expression + token->loc.l);
		return FAIL;
	}

	strval = zbx_strdup(NULL, arg->data.str);

	str_alloc = str_len = strlen(strval) + 1;
	zbx_replace_mem_dyn(&strval, &str_alloc, &str_len, start->data.ui64 - 1, len->data.ui64, replacement->data.str,
			strlen(replacement->data.str));

	zbx_variant_set_str(&value, strval);
	eval_function_return(4, &value, output);

	return SUCCEED;
}

/******************************************************************************
 *                                                                            *
 * Function: eval_execute_function_replace                                    *
 *                                                                            *
 * Purpose: evaluate replace() function                                       *
 *                                                                            *
 * Parameters: ctx    - [IN] the evaluation context                           *
 *             token  - [IN] the function token                               *
 *             output - [IN/OUT] the output value stack                       *
 *             error  - [OUT] the error message in the case of failure        *
 *                                                                            *
 * Return value: SUCCEED - function evaluation succeeded                      *
 *               FAIL    - otherwise                                          *
 *                                                                            *
 ******************************************************************************/
static int	eval_execute_function_replace(const zbx_eval_context_t *ctx, const zbx_eval_token_t *token,
		zbx_vector_var_t *output, char **error)
{
	int		ret;
	zbx_variant_t	*arg, *pattern, *replacement, value;
	char		*strval, *p;
	size_t		pattern_len, replacement_len;

	if (3 != token->opt)
	{
		*error = zbx_dsprintf(*error, "invalid number of arguments for function at \"%s\"",
				ctx->expression + token->loc.l);
		return FAIL;
	}

	if (UNKNOWN != (ret = eval_validate_function_args(ctx, token, output, error)))
		return ret;

	arg = &output->values[output->values_num - 3];
	pattern = &output->values[output->values_num - 2];
	replacement = &output->values[output->values_num - 1];

	if (SUCCEED != zbx_variant_convert(arg, ZBX_VARIANT_STR))
	{
		*error = zbx_dsprintf(*error, "invalid function first argument at \"%s\"",
				ctx->expression + token->loc.l);
		return FAIL;
	}

	if (SUCCEED != zbx_variant_convert(pattern, ZBX_VARIANT_STR))
	{
		*error = zbx_dsprintf(*error, "invalid function second argument at \"%s\"",
				ctx->expression + token->loc.l);
		return FAIL;
	}

	if (SUCCEED != zbx_variant_convert(replacement, ZBX_VARIANT_STR))
	{
		*error = zbx_dsprintf(*error, "invalid function third argument at \"%s\"",
				ctx->expression + token->loc.l);
		return FAIL;
	}

	strval = zbx_strdup(NULL, arg->data.str);
	pattern_len = strlen(pattern->data.str);

	if (0 < pattern_len)
	{
		replacement_len = strlen(replacement->data.str);

		while (NULL != (p = strstr(strval, pattern->data.str)))
		{
			size_t	str_alloc, str_len;

			str_alloc = str_len = strlen(strval) + 1;
			zbx_replace_mem_dyn(&strval, &str_alloc, &str_len, (size_t)(p - strval), pattern_len,
					replacement->data.str, replacement_len);
		}
	}

	zbx_variant_set_str(&value, strval);
	eval_function_return(3, &value, output);

	return SUCCEED;
}

/******************************************************************************
 *                                                                            *
 * Function: eval_execute_function_repeat                                     *
 *                                                                            *
 * Purpose: evaluate repeat() function                                        *
 *                                                                            *
 * Parameters: ctx    - [IN] the evaluation context                           *
 *             token  - [IN] the function token                               *
 *             output - [IN/OUT] the output value stack                       *
 *             error  - [OUT] the error message in the case of failure        *
 *                                                                            *
 * Return value: SUCCEED - function evaluation succeeded                      *
 *               FAIL    - otherwise                                          *
 *                                                                            *
 ******************************************************************************/
static int	eval_execute_function_repeat(const zbx_eval_context_t *ctx, const zbx_eval_token_t *token,
		zbx_vector_var_t *output, char **error)
{
	int		ret;
	zbx_variant_t	*str, *num, value;
	char		*strval = NULL;
	zbx_uint64_t	i;

	if (2 != token->opt)
	{
		*error = zbx_dsprintf(*error, "invalid number of arguments for function at \"%s\"",
				ctx->expression + token->loc.l);
		return FAIL;
	}

	if (UNKNOWN != (ret = eval_validate_function_args(ctx, token, output, error)))
		return ret;

	str = &output->values[output->values_num - 2];
	num = &output->values[output->values_num - 1];

	if (SUCCEED != zbx_variant_convert(str, ZBX_VARIANT_STR))
	{
		*error = zbx_dsprintf(*error, "invalid function first argument at \"%s\"",
				ctx->expression + token->loc.l);
		return FAIL;
	}

	if (SUCCEED != zbx_variant_convert(num, ZBX_VARIANT_UI64))
	{
		*error = zbx_dsprintf(*error, "function argument \"%s\" is not an unsigned integer value at \"%s\"",
				zbx_variant_value_desc(num), ctx->expression + token->loc.l);
		return FAIL;
	}

	if (num->data.ui64 * strlen(str->data.str) >= MAX_STRING_LEN)
	{
		*error = zbx_dsprintf(*error, "maximum allowed string length (%d) exceeded: " ZBX_FS_UI64,
				MAX_STRING_LEN, num->data.ui64 * strlen(str->data.str));
		return FAIL;
	}

	for (i = num->data.ui64; i > 0; i--)
		strval = zbx_strdcat(strval, str->data.str);

	if (NULL == strval)
		strval = zbx_strdup(NULL, "");

	zbx_variant_set_str(&value, strval);
	eval_function_return(2, &value, output);

	return SUCCEED;
}

/******************************************************************************
 *                                                                            *
 * Function: eval_execute_function_bytelength                                 *
 *                                                                            *
 * Purpose: evaluate bytelength() function                                    *
 *                                                                            *
 * Parameters: ctx    - [IN] the evaluation context                           *
 *             token  - [IN] the function token                               *
 *             output - [IN/OUT] the output value stack                       *
 *             error  - [OUT] the error message in the case of failure        *
 *                                                                            *
 * Return value: SUCCEED - function evaluation succeeded                      *
 *               FAIL    - otherwise                                          *
 *                                                                            *
 ******************************************************************************/
static int	eval_execute_function_bytelength(const zbx_eval_context_t *ctx, const zbx_eval_token_t *token,
		zbx_vector_var_t *output, char **error)
{
	int		ret;
	zbx_variant_t	*arg, value;

	if (1 != token->opt)
	{
		*error = zbx_dsprintf(*error, "invalid number of arguments for function at \"%s\"",
				ctx->expression + token->loc.l);
		return FAIL;
	}

	if (UNKNOWN != (ret = eval_validate_function_args(ctx, token, output, error)))
		return ret;

	arg = &output->values[output->values_num - 1];

	if (SUCCEED == zbx_variant_convert(arg, ZBX_VARIANT_UI64))
	{
		zbx_uint64_t	byte = __UINT64_C(0xFF00000000000000);
		int		i;

		for (i = 8; i > 0; i--)
		{
			if (byte & arg->data.ui64)
				break;

			byte = byte >> 8;
		}

		zbx_variant_set_dbl(&value, i);
	}
	else if (SUCCEED == zbx_variant_convert(arg, ZBX_VARIANT_DBL))
	{
		zbx_variant_set_dbl(&value, sizeof(arg->data.dbl));
	}
	else if (SUCCEED != zbx_variant_convert(arg, ZBX_VARIANT_STR))
	{
		*error = zbx_dsprintf(*error, "invalid function argument at \"%s\"", ctx->expression + token->loc.l);
		return FAIL;
	}
	else
		zbx_variant_set_dbl(&value, strlen(arg->data.str));

	eval_function_return(1, &value, output);

	return SUCCEED;
}

/******************************************************************************
 *                                                                            *
 * Function: eval_execute_function_bitlength                                  *
 *                                                                            *
 * Purpose: evaluate bitlength() function                                     *
 *                                                                            *
 * Parameters: ctx    - [IN] the evaluation context                           *
 *             token  - [IN] the function token                               *
 *             output - [IN/OUT] the output value stack                       *
 *             error  - [OUT] the error message in the case of failure        *
 *                                                                            *
 * Return value: SUCCEED - function evaluation succeeded                      *
 *               FAIL    - otherwise                                          *
 *                                                                            *
 ******************************************************************************/
static int	eval_execute_function_bitlength(const zbx_eval_context_t *ctx, const zbx_eval_token_t *token,
		zbx_vector_var_t *output, char **error)
{
	int		ret;
	zbx_variant_t	*arg, value;

	if (1 != token->opt)
	{
		*error = zbx_dsprintf(*error, "invalid number of arguments for function at \"%s\"",
				ctx->expression + token->loc.l);
		return FAIL;
	}

	if (UNKNOWN != (ret = eval_validate_function_args(ctx, token, output, error)))
		return ret;

	arg = &output->values[output->values_num - 1];

	if (SUCCEED == zbx_variant_convert(arg, ZBX_VARIANT_UI64))
	{
		int	i, bits;

		bits = sizeof(uint64_t) * 8;

		for (i = bits - 1; i >= 0; i--)
		{
			if (__UINT64_C(1) << i & arg->data.ui64)
				break;
		}

		zbx_variant_set_dbl(&value, ++i);
	}
	else if (SUCCEED == zbx_variant_convert(arg, ZBX_VARIANT_DBL))
	{
		zbx_variant_set_dbl(&value, sizeof(arg->data.dbl) * 8);
	}
	else if (SUCCEED != zbx_variant_convert(arg, ZBX_VARIANT_STR))
	{
		*error = zbx_dsprintf(*error, "invalid function argument at \"%s\"", ctx->expression + token->loc.l);
		return FAIL;
	}
	else
		zbx_variant_set_dbl(&value, strlen(arg->data.str) * 8);

	eval_function_return(1, &value, output);

	return SUCCEED;
}

/******************************************************************************
 *                                                                            *
 * Function: eval_execute_function_char                                       *
 *                                                                            *
 * Purpose: evaluate char() function                                          *
 *                                                                            *
 * Parameters: ctx    - [IN] the evaluation context                           *
 *             token  - [IN] the function token                               *
 *             output - [IN/OUT] the output value stack                       *
 *             error  - [OUT] the error message in the case of failure        *
 *                                                                            *
 * Return value: SUCCEED - function evaluation succeeded                      *
 *               FAIL    - otherwise                                          *
 *                                                                            *
 ******************************************************************************/
static int	eval_execute_function_char(const zbx_eval_context_t *ctx, const zbx_eval_token_t *token,
		zbx_vector_var_t *output, char **error)
{
	int		ret;
	zbx_variant_t	*arg, value;

	if (1 != token->opt)
	{
		*error = zbx_dsprintf(*error, "invalid number of arguments for function at \"%s\"",
				ctx->expression + token->loc.l);
		return FAIL;
	}

	if (UNKNOWN != (ret = eval_validate_function_args(ctx, token, output, error)))
		return ret;

	arg = &output->values[output->values_num - 1];

	if (SUCCEED != zbx_variant_convert(arg, ZBX_VARIANT_UI64) || 255 < arg->data.ui64)
	{
		*error = zbx_dsprintf(*error, "invalid function argument at \"%s\"", ctx->expression + token->loc.l);
		return FAIL;
	}

	zbx_variant_set_str(&value, zbx_dsprintf(NULL, "%c", (char)arg->data.ui64));
	eval_function_return(1, &value, output);

	return SUCCEED;
}

/******************************************************************************
 *                                                                            *
 * Function: eval_execute_function_ascii                                      *
 *                                                                            *
 * Purpose: evaluate ascii() function                                         *
 *                                                                            *
 * Parameters: ctx    - [IN] the evaluation context                           *
 *             token  - [IN] the function token                               *
 *             output - [IN/OUT] the output value stack                       *
 *             error  - [OUT] the error message in the case of failure        *
 *                                                                            *
 * Return value: SUCCEED - function evaluation succeeded                      *
 *               FAIL    - otherwise                                          *
 *                                                                            *
 ******************************************************************************/
static int	eval_execute_function_ascii(const zbx_eval_context_t *ctx, const zbx_eval_token_t *token,
		zbx_vector_var_t *output, char **error)
{
	int		ret;
	zbx_variant_t	*arg, value;

	if (1 != token->opt)
	{
		*error = zbx_dsprintf(*error, "invalid number of arguments for function at \"%s\"",
				ctx->expression + token->loc.l);
		return FAIL;
	}

	if (UNKNOWN != (ret = eval_validate_function_args(ctx, token, output, error)))
		return ret;

	arg = &output->values[output->values_num - 1];

	if (SUCCEED != zbx_variant_convert(arg, ZBX_VARIANT_STR) || 0 > *arg->data.str || 255 < *arg->data.str)
	{
		*error = zbx_dsprintf(*error, "invalid function argument at \"%s\"", ctx->expression + token->loc.l);
		return FAIL;
	}

	zbx_variant_set_ui64(&value, *(zbx_uint64_t*)arg->data.str);
	eval_function_return(1, &value, output);

	return SUCCEED;
}

/******************************************************************************
 *                                                                            *
 * Function: eval_execute_function_between                                    *
 *                                                                            *
 * Purpose: evaluate between() function                                       *
 *                                                                            *
 * Parameters: ctx    - [IN] the evaluation context                           *
 *             token  - [IN] the function token                               *
 *             output - [IN/OUT] the output value stack                       *
 *             error  - [OUT] the error message in the case of failure        *
 *                                                                            *
 * Return value: SUCCEED - function evaluation succeeded                      *
 *               FAIL    - otherwise                                          *
 *                                                                            *
 ******************************************************************************/
static int	eval_execute_function_between(const zbx_eval_context_t *ctx, const zbx_eval_token_t *token,
		zbx_vector_var_t *output, char **error)
{
	int		i, ret;
	double		between;
	zbx_variant_t	value;

	if (3 != token->opt)
	{
		*error = zbx_dsprintf(*error, "invalid number of arguments for function at \"%s\"",
				ctx->expression + token->loc.l);
		return FAIL;
	}

	if (UNKNOWN != (ret = eval_prepare_math_function_args(ctx, token, output, error)))
		return ret;

	i = output->values_num - token->opt;
	between = output->values[i++].data.dbl;

	if (output->values[i++].data.dbl <= between && between <= output->values[i].data.dbl)
		zbx_variant_set_dbl(&value, 1);
	else
		zbx_variant_set_dbl(&value, 0);

	eval_function_return(3, &value, output);

	return SUCCEED;
}

/******************************************************************************
 *                                                                            *
 * Function: eval_execute_function_in                                         *
 *                                                                            *
 * Purpose: evaluate in() function                                            *
 *                                                                            *
 * Parameters: ctx    - [IN] the evaluation context                           *
 *             token  - [IN] the function token                               *
 *             output - [IN/OUT] the output value stack                       *
 *             error  - [OUT] the error message in the case of failure        *
 *                                                                            *
 * Return value: SUCCEED - function evaluation succeeded                      *
 *               FAIL    - otherwise                                          *
 *                                                                            *
 ******************************************************************************/
static int	eval_execute_function_in(const zbx_eval_context_t *ctx, const zbx_eval_token_t *token,
		zbx_vector_var_t *output, char **error)
{
	zbx_variant_t	value;
	int		i, ret;

	if (2 > token->opt)
	{
		*error = zbx_dsprintf(*error, "invalid number of arguments for function at \"%s\"",
				ctx->expression + token->loc.l);
		return FAIL;
	}

	zbx_variant_set_dbl(&value, 0);

	if (UNKNOWN != (ret = eval_prepare_math_function_args(ctx, token, output, error)))
	{
		zbx_variant_t	*cmpr, *arg;

		if (UNKNOWN != (ret = eval_validate_function_args(ctx, token, output, error)))
			return ret;

		cmpr = &output->values[0];

		if (SUCCEED != zbx_variant_convert(cmpr, ZBX_VARIANT_STR))
		{
			*error = zbx_dsprintf(*error, "invalid function first argument at \"%s\"",
					ctx->expression + token->loc.l);
			return FAIL;
		}

		for (i = 1; i < output->values_num; i++)
		{
			arg = &output->values[i];

			if (SUCCEED != zbx_variant_convert(arg, ZBX_VARIANT_STR))
			{
				*error = zbx_dsprintf(*error, "invalid function argument \"%s\" at \"%s\"",
						zbx_variant_value_desc(arg), ctx->expression + token->loc.l);
				return FAIL;
			}

			if (0 == strcmp(cmpr->data.str, arg->data.str))
			{
				zbx_variant_set_dbl(&value, 1);
				break;
			}
		}
	}
	else
	{
		double	in;

		i = output->values_num - token->opt;
		in = output->values[i++].data.dbl;

		for (; i < output->values_num; i++)
		{
			if (in == output->values[i].data.dbl)
			{
				zbx_variant_set_dbl(&value, 1);
				break;
			}
		}
	}

	eval_function_return(token->opt, &value, output);

	return SUCCEED;
}

/******************************************************************************
 *                                                                            *
 * Function: eval_execute_cb_function                                         *
 *                                                                            *
 * Purpose: evaluate function by calling custom callback (if configured)      *
 *                                                                            *
 * Parameters: ctx        - [IN] the evaluation context                       *
 *             token      - [IN] the function token                           *
 *             functio_cb - [IN] the callback function                        *
 *             output     - [IN/OUT] the output value stack                   *
 *             error      - [OUT] the error message in the case of failure    *
 *                                                                            *
 * Return value: SUCCEED - the function was executed successfully             *
 *               FAIL    - otherwise                                          *
 *                                                                            *
 ******************************************************************************/
static int	eval_execute_cb_function(const zbx_eval_context_t *ctx, const zbx_eval_token_t *token,
		zbx_eval_function_cb_t function_cb, zbx_vector_var_t *output, char **error)
{
	zbx_variant_t	value, *args;
	char		*errmsg = NULL;

	args = (0 == token->opt ? NULL : &output->values[output->values_num - token->opt]);

	if (SUCCEED != function_cb(ctx->expression + token->loc.l, token->loc.r - token->loc.l + 1,
			token->opt, args, ctx->data_cb, &ctx->ts, &value, &errmsg))
	{
		*error = zbx_dsprintf(*error, "%s at \"%s\".", errmsg, ctx->expression + token->loc.l);
		zbx_free(errmsg);

		if (0 == (ctx->rules & ZBX_EVAL_PROCESS_ERROR))
			return FAIL;

		zbx_variant_set_error(&value, *error);
		*error = NULL;
	}

	eval_function_return(token->opt, &value, output);

	return SUCCEED;
}

/******************************************************************************
 *                                                                            *
 * Function: eval_execute_common_function                                     *
 *                                                                            *
 * Purpose: evaluate common function                                          *
 *                                                                            *
 * Parameters: ctx    - [IN] the evaluation context                           *
 *             token  - [IN] the function token                               *
 *             output - [IN/OUT] the output value stack                       *
 *             error  - [OUT] the error message in the case of failure        *
 *                                                                            *
 * Return value: SUCCEED - the function was executed successfully             *
 *               FAIL    - otherwise                                          *
 *                                                                            *
 ******************************************************************************/
static int	eval_execute_common_function(const zbx_eval_context_t *ctx, const zbx_eval_token_t *token,
		zbx_vector_var_t *output, char **error)
{
	if ((zbx_uint32_t)output->values_num < token->opt)
	{
		*error = zbx_dsprintf(*error, "not enough arguments for function at \"%s\"",
				ctx->expression + token->loc.l);
		return FAIL;
	}

	if (SUCCEED == eval_compare_token(ctx, &token->loc, "min", ZBX_CONST_STRLEN("min")))
		return eval_execute_function_min(ctx, token, output, error);
	if (SUCCEED == eval_compare_token(ctx, &token->loc, "max", ZBX_CONST_STRLEN("max")))
		return eval_execute_function_max(ctx, token, output, error);
	if (SUCCEED == eval_compare_token(ctx, &token->loc, "sum", ZBX_CONST_STRLEN("sum")))
		return eval_execute_function_sum(ctx, token, output, error);
	if (SUCCEED == eval_compare_token(ctx, &token->loc, "avg", ZBX_CONST_STRLEN("avg")))
		return eval_execute_function_avg(ctx, token, output, error);
	if (SUCCEED == eval_compare_token(ctx, &token->loc, "abs", ZBX_CONST_STRLEN("abs")))
		return eval_execute_function_abs(ctx, token, output, error);
	if (SUCCEED == eval_compare_token(ctx, &token->loc, "length", ZBX_CONST_STRLEN("length")))
		return eval_execute_function_length(ctx, token, output, error);
	if (SUCCEED == eval_compare_token(ctx, &token->loc, "date", ZBX_CONST_STRLEN("date")))
		return eval_execute_function_date(ctx, token, output, error);
	if (SUCCEED == eval_compare_token(ctx, &token->loc, "time", ZBX_CONST_STRLEN("time")))
		return eval_execute_function_time(ctx, token, output, error);
	if (SUCCEED == eval_compare_token(ctx, &token->loc, "now", ZBX_CONST_STRLEN("now")))
		return eval_execute_function_now(ctx, token, output, error);
	if (SUCCEED == eval_compare_token(ctx, &token->loc, "dayofweek", ZBX_CONST_STRLEN("dayofweek")))
		return eval_execute_function_dayofweek(ctx, token, output, error);
	if (SUCCEED == eval_compare_token(ctx, &token->loc, "dayofmonth", ZBX_CONST_STRLEN("dayofmonth")))
		return eval_execute_function_dayofmonth(ctx, token, output, error);
	if (SUCCEED == eval_compare_token(ctx, &token->loc, "bitand", ZBX_CONST_STRLEN("bitand")))
		return eval_execute_function_bitwise(ctx, token, FUNCTION_OPTYPE_BIT_AND, output, error);
	if (SUCCEED == eval_compare_token(ctx, &token->loc, "bitor", ZBX_CONST_STRLEN("bitor")))
		return eval_execute_function_bitwise(ctx, token, FUNCTION_OPTYPE_BIT_OR, output, error);
	if (SUCCEED == eval_compare_token(ctx, &token->loc, "bitxor", ZBX_CONST_STRLEN("bitxor")))
		return eval_execute_function_bitwise(ctx, token, FUNCTION_OPTYPE_BIT_XOR, output, error);
	if (SUCCEED == eval_compare_token(ctx, &token->loc, "bitlshift", ZBX_CONST_STRLEN("bitlshift")))
		return eval_execute_function_bitwise(ctx, token, FUNCTION_OPTYPE_BIT_LSHIFT, output, error);
	if (SUCCEED == eval_compare_token(ctx, &token->loc, "bitrshift", ZBX_CONST_STRLEN("bitrshift")))
		return eval_execute_function_bitwise(ctx, token, FUNCTION_OPTYPE_BIT_RSHIFT, output, error);
	if (SUCCEED == eval_compare_token(ctx, &token->loc, "bitnot", ZBX_CONST_STRLEN("bitnot")))
		return eval_execute_function_bitnot(ctx, token, output, error);
	if (SUCCEED == eval_compare_token(ctx, &token->loc, "between", ZBX_CONST_STRLEN("between")))
		return eval_execute_function_between(ctx, token, output, error);
	if (SUCCEED == eval_compare_token(ctx, &token->loc, "in", ZBX_CONST_STRLEN("in")))
		return eval_execute_function_in(ctx, token, output, error);
	if (SUCCEED == eval_compare_token(ctx, &token->loc, "ascii", ZBX_CONST_STRLEN("ascii")))
		return eval_execute_function_ascii(ctx, token, output, error);
	if (SUCCEED == eval_compare_token(ctx, &token->loc, "char", ZBX_CONST_STRLEN("char")))
		return eval_execute_function_char(ctx, token, output, error);
	if (SUCCEED == eval_compare_token(ctx, &token->loc, "left", ZBX_CONST_STRLEN("left")))
		return eval_execute_function_left(ctx, token, output, error);
	if (SUCCEED == eval_compare_token(ctx, &token->loc, "right", ZBX_CONST_STRLEN("right")))
		return eval_execute_function_right(ctx, token, output, error);
	if (SUCCEED == eval_compare_token(ctx, &token->loc, "mid", ZBX_CONST_STRLEN("mid")))
		return eval_execute_function_mid(ctx, token, output, error);
	if (SUCCEED == eval_compare_token(ctx, &token->loc, "bitlength", ZBX_CONST_STRLEN("bitlength")))
		return eval_execute_function_bitlength(ctx, token, output, error);
	if (SUCCEED == eval_compare_token(ctx, &token->loc, "bytelength", ZBX_CONST_STRLEN("bytelength")))
		return eval_execute_function_bytelength(ctx, token, output, error);
	if (SUCCEED == eval_compare_token(ctx, &token->loc, "concat", ZBX_CONST_STRLEN("concat")))
		return eval_execute_function_concat(ctx, token, output, error);
	if (SUCCEED == eval_compare_token(ctx, &token->loc, "insert", ZBX_CONST_STRLEN("insert")))
		return eval_execute_function_insert(ctx, token, output, error);
	if (SUCCEED == eval_compare_token(ctx, &token->loc, "replace", ZBX_CONST_STRLEN("replace")))
		return eval_execute_function_replace(ctx, token, output, error);
	if (SUCCEED == eval_compare_token(ctx, &token->loc, "repeat", ZBX_CONST_STRLEN("repeat")))
		return eval_execute_function_repeat(ctx, token, output, error);
	if (SUCCEED == eval_compare_token(ctx, &token->loc, "ltrim", ZBX_CONST_STRLEN("ltrim")))
		return eval_execute_function_trim(ctx, token, FUNCTION_OPTYPE_TRIM_LEFT, output, error);
	if (SUCCEED == eval_compare_token(ctx, &token->loc, "rtrim", ZBX_CONST_STRLEN("rtrim")))
		return eval_execute_function_trim(ctx, token, FUNCTION_OPTYPE_TRIM_RIGHT, output, error);
	if (SUCCEED == eval_compare_token(ctx, &token->loc, "trim", ZBX_CONST_STRLEN("trim")))
		return eval_execute_function_trim(ctx, token, FUNCTION_OPTYPE_TRIM_ALL, output, error);

	if (NULL != ctx->common_func_cb)
		return eval_execute_cb_function(ctx, token, ctx->common_func_cb, output, error);

	*error = zbx_dsprintf(*error, "Unknown function at \"%s\".", ctx->expression + token->loc.l);
	return FAIL;
}

/******************************************************************************
 *                                                                            *
 * Function: eval_execute_history_function                                    *
 *                                                                            *
 * Purpose: evaluate history function                                         *
 *                                                                            *
 * Parameters: ctx    - [IN] the evaluation context                           *
 *             token  - [IN] the function token                               *
 *             output - [IN/OUT] the output value stack                       *
 *             error  - [OUT] the error message in the case of failure        *
 *                                                                            *
 * Return value: SUCCEED - the function was executed successfully             *
 *               FAIL    - otherwise                                          *
 *                                                                            *
 ******************************************************************************/
static int	eval_execute_history_function(const zbx_eval_context_t *ctx, const zbx_eval_token_t *token,
		zbx_vector_var_t *output, char **error)
{
	if ((zbx_uint32_t)output->values_num < token->opt)
	{
		*error = zbx_dsprintf(*error, "not enough arguments for function at \"%s\"",
				ctx->expression + token->loc.l);
		return FAIL;
	}

	if (NULL != ctx->history_func_cb)
		return eval_execute_cb_function(ctx, token, ctx->history_func_cb, output, error);

	*error = zbx_dsprintf(*error, "Unknown function at \"%s\".", ctx->expression + token->loc.l);
	return FAIL;
}

/******************************************************************************
 *                                                                            *
 * Function: eval_throw_exception                                             *
 *                                                                            *
 * Purpose: throw exception by returning the specified error                  *
 *                                                                            *
 * Parameters: output - [IN/OUT] the output value stack                       *
 *             error  - [OUT] the error message in the case of failure        *
 *                                                                            *
 ******************************************************************************/
static void	eval_throw_exception(zbx_vector_var_t *output, char **error)
{
	zbx_variant_t	*arg;

	if (0 == output->values_num)
	{
		*error = zbx_strdup(*error, "exception must have one argument");
		return;
	}

	arg = &output->values[output->values_num - 1];
	zbx_variant_convert(arg, ZBX_VARIANT_STR);
	*error = arg->data.str;
	zbx_variant_set_none(arg);
}

/******************************************************************************
 *                                                                            *
 * Function: eval_execute                                                     *
 *                                                                            *
 * Purpose: evaluate pre-parsed expression                                    *
 *                                                                            *
 * Parameters: ctx   - [IN] the evaluation context                            *
 *             value - [OUT] the resulting value                              *
 *             error - [OUT] the error message in the case of failure         *
 *                                                                            *
 * Return value: SUCCEED - the expression was evaluated successfully          *
 *               FAIL    - otherwise                                          *
 *                                                                            *
 ******************************************************************************/
static int	eval_execute(const zbx_eval_context_t *ctx, zbx_variant_t *value, char **error)
{
	zbx_vector_var_t	output;
	int			i, ret = FAIL;
	char			*errmsg = NULL;

	zbx_vector_var_create(&output);

	for (i = 0; i < ctx->stack.values_num; i++)
	{
		zbx_eval_token_t	*token = &ctx->stack.values[i];

		if (0 != (token->type & ZBX_EVAL_CLASS_OPERATOR1))
		{
			if (SUCCEED != eval_execute_op_unary(ctx, token, &output, &errmsg))
				goto out;
		}
		else if (0 != (token->type & ZBX_EVAL_CLASS_OPERATOR2))
		{
			if (SUCCEED != eval_execute_op_binary(ctx, token, &output, &errmsg))
				goto out;
		}
		else
		{
			switch (token->type)
			{
				case ZBX_EVAL_TOKEN_NOP:
					break;
				case ZBX_EVAL_TOKEN_VAR_NUM:
				case ZBX_EVAL_TOKEN_VAR_STR:
				case ZBX_EVAL_TOKEN_VAR_MACRO:
				case ZBX_EVAL_TOKEN_VAR_USERMACRO:
					if (SUCCEED != eval_execute_push_value(ctx, token, &output, &errmsg))
						goto out;
					break;
				case ZBX_EVAL_TOKEN_ARG_QUERY:
				case ZBX_EVAL_TOKEN_ARG_PERIOD:
					if (SUCCEED != eval_execute_push_value(ctx, token, &output, &errmsg))
						goto out;
					break;
				case ZBX_EVAL_TOKEN_ARG_NULL:
					eval_execute_push_null(&output);
					break;
				case ZBX_EVAL_TOKEN_FUNCTION:
					if (SUCCEED != eval_execute_common_function(ctx, token, &output, &errmsg))
						goto out;
					break;
				case ZBX_EVAL_TOKEN_HIST_FUNCTION:
					if (SUCCEED != eval_execute_history_function(ctx, token, &output, &errmsg))
						goto out;
					break;
				case ZBX_EVAL_TOKEN_FUNCTIONID:
					if (ZBX_VARIANT_NONE == token->value.type)
					{
						errmsg = zbx_strdup(errmsg, "trigger history functions must be"
								" pre-calculated");
						goto out;
					}
					if (SUCCEED != eval_execute_push_value(ctx, token, &output, &errmsg))
						goto out;
					break;
				case ZBX_EVAL_TOKEN_EXCEPTION:
					eval_throw_exception(&output, &errmsg);
					goto out;
				default:
					errmsg = zbx_dsprintf(errmsg, "unknown token at \"%s\"",
							ctx->expression + token->loc.l);
					goto out;
			}
		}
	}

	if (1 != output.values_num)
	{
		errmsg = zbx_strdup(errmsg, "output stack after expression execution must contain one value");
		goto out;
	}

	if (ZBX_VARIANT_ERR == output.values[0].type)
	{
		errmsg = zbx_strdup(errmsg, output.values[0].data.err);
		goto out;
	}

	*value = output.values[0];
	output.values_num = 0;

	ret = SUCCEED;
out:
	if (SUCCEED != ret)
	{
		if (0 != islower(*errmsg))
		{
			*error = zbx_dsprintf(NULL, "Cannot evaluate expression: %s", errmsg);
			zbx_free(errmsg);
		}
		else
			*error = errmsg;
	}

	for (i = 0; i < output.values_num; i++)
		zbx_variant_clear(&output.values[i]);

	zbx_vector_var_destroy(&output);

	return ret;
}

/******************************************************************************
 *                                                                            *
 * Function: eval_init_execute_context                                        *
 *                                                                            *
 * Purpose: initialize execution context                                      *
 *                                                                            *
 * Parameters: ctx             - [IN] the evaluation context                  *
 *             ts              - [IN] the timestamp of the execution time     *
 *             common_func_cb  - [IN] the common function callback (optional) *
 *             history_func_cb - [IN] the history function callback (optional)*
 *             data_cb         - [IN] the caller data to be passed to callback*
 *                                    functions                               *
 *                                                                            *
 ******************************************************************************/
static void	eval_init_execute_context(zbx_eval_context_t *ctx, const zbx_timespec_t *ts,
		zbx_eval_function_cb_t common_func_cb, zbx_eval_function_cb_t history_func_cb, void *data_cb)
{
	ctx->common_func_cb = common_func_cb;
	ctx->history_func_cb = history_func_cb;
	ctx->data_cb = data_cb;

	if (NULL == ts)
		ctx->ts.sec = ctx->ts.ns = 0;
	else
		ctx->ts = *ts;
}

/******************************************************************************
 *                                                                            *
 * Function: zbx_eval_execute                                                 *
 *                                                                            *
 * Purpose: evaluate parsed expression                                        *
 *                                                                            *
 * Parameters: ctx   - [IN] the evaluation context                            *
 *             ts    - [IN] the timestamp of the execution time               *
 *             value - [OUT] the resulting value                              *
 *             error - [OUT] the error message in the case of failure         *
 *                                                                            *
 * Return value: SUCCEED - the expression was evaluated successfully          *
 *               FAIL    - otherwise                                          *
 *                                                                            *
 ******************************************************************************/
int	zbx_eval_execute(zbx_eval_context_t *ctx, const zbx_timespec_t *ts, zbx_variant_t *value, char **error)
{
	eval_init_execute_context(ctx, ts, NULL, NULL, NULL);

	return eval_execute(ctx, value, error);
}

/******************************************************************************
 *                                                                            *
 * Function: zbx_eval_execute_ext                                             *
 *                                                                            *
 * Purpose: evaluate parsed expression with callback for custom function      *
 *          processing                                                        *
 *                                                                            *
 * Parameters: ctx             - [IN] the evaluation context                  *
 *             ts              - [IN] the timestamp of the execution time     *
 *             common_func_cb  - [IN] the common function callback (optional) *
 *             history_func_cb - [IN] the history function callback (optional)*
 *             value           - [OUT] the resulting value                    *
 *             error           - [OUT] the error message                      *
 *                                                                            *
 * Return value: SUCCEED - the expression was evaluated successfully          *
 *               FAIL    - otherwise                                          *
 *                                                                            *
 * Comments: The callback will be called for unsupported math and all history *
 *           functions.                                                       *
 *                                                                            *
 ******************************************************************************/
int	zbx_eval_execute_ext(zbx_eval_context_t *ctx, const zbx_timespec_t *ts, zbx_eval_function_cb_t common_func_cb,
		zbx_eval_function_cb_t history_func_cb, void *data, zbx_variant_t *value, char **error)
{
	eval_init_execute_context(ctx, ts, common_func_cb, history_func_cb, data);

	return eval_execute(ctx, value, error);
}
