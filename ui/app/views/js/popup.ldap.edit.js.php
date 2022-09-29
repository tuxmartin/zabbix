<?php declare(strict_types = 0);
/*
** Zabbix
** Copyright (C) 2001-2022 Zabbix SIA
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


?>

window.ldap_edit_popup = new class {

	constructor() {
		this.overlay = null;
		this.dialogue = null;
		this.form = null;
		this.advanced_chbox = null;
	}

	init({provision_groups, provision_media}) {
		this.overlay = overlays_stack.getById('ldap_edit');
		this.dialogue = this.overlay.$dialogue[0];
		this.form = this.overlay.$dialogue.$body[0].querySelector('form');
		this.advanced_chbox = document.getElementById('advanced_configuration');
		this.allow_jit_chbox = document.getElementById('provision_status');
		this.provision_groups_table = document.getElementById('ldap-user-groups-table');

		this.toggleAdvancedConfiguration(this.advanced_chbox.checked);
		this.toggleAllowJitProvisioning(this.allow_jit_chbox.checked);

		this._addEventListeners();
		this._renderProvisionGroups(provision_groups);
		this._renderProvisionMedia(provision_media);
		this._initSortable();

		if (document.getElementById('bind-password-btn') !== null) {
			document.getElementById('bind-password-btn').addEventListener('click', this.showPasswordField);
		}
	}

	_addEventListeners() {
		this.advanced_chbox.addEventListener('change', (e) => {
			this.toggleAdvancedConfiguration(e.target.checked);
		});

		this.allow_jit_chbox.addEventListener('change', (e) => {
			this.toggleAllowJitProvisioning(e.target.checked);
		});

		this.provision_groups_table.addEventListener('click', (e) => {
			if (e.target.classList.contains('js-add')) {
				this.editProvisionGroup();
			}
			else if (e.target.classList.contains('js-edit')) {
				this.editProvisionGroup(e.target.closest('tr'));
			}
			else if (e.target.classList.contains('js-remove')) {
				e.target.closest('tr').remove();
			}
			else if (e.target.classList.contains('js-enabled')) {
				this.toggleFallbackStatus(<?= GROUP_MAPPING_FALLBACK_OFF ?>);
			}
			else if (e.target.classList.contains('js-disabled')) {
				this.toggleFallbackStatus(<?= GROUP_MAPPING_FALLBACK_ON ?>);
			}
		});

		document
			.getElementById('ldap-media-type-mapping-table')
			.addEventListener('click', (e) => {
				if (e.target.classList.contains('js-add')) {
					this.editProvisionMediaType();
				}
				else if (e.target.classList.contains('js-edit')) {
					this.editProvisionMediaType(e.target.closest('tr'));
				}
				else if (e.target.classList.contains('js-remove')) {
					e.target.closest('tr').remove()
				}
			});
	}

	toggleAdvancedConfiguration(checked) {
		for (const element of this.form.querySelectorAll('.advanced-configuration')) {
			element.classList.toggle('<?= ZBX_STYLE_DISPLAY_NONE ?>', !checked);
		}
	}

	toggleAllowJitProvisioning(checked) {
		for (const element of this.form.querySelectorAll('.allow-jit-provisioning')) {
			element.classList.toggle('<?= ZBX_STYLE_DISPLAY_NONE ?>', !checked);
		}
	}

	toggleFallbackStatus(status) {
		const row = this.dialogue.querySelector('[data-row_fallback]');
		const btn = row.querySelector('.btn-link');

		if (status == <?= GROUP_MAPPING_FALLBACK_ON ?>) {
			row.querySelector('[name$="[fallback_status]"]').value = status;
			btn.classList.replace('<?= ZBX_STYLE_RED ?>', '<?= ZBX_STYLE_GREEN ?>');
			btn.classList.replace('js-disabled', 'js-enabled');
			btn.innerText = '<?= _('Enabled') ?>';
		}
		else {
			row.querySelector('[name$="[fallback_status]"]').value = status;
			btn.classList.replace('<?= ZBX_STYLE_GREEN ?>', '<?= ZBX_STYLE_RED ?>');
			btn.classList.replace('js-enabled', 'js-disabled');
			btn.innerText = '<?= _('Disabled') ?>';
		}
	}

	_initSortable() {
		$(this.provision_groups_table).sortable({
			items: 'tbody tr.sortable',
			axis: 'y',
			containment: 'parent',
			cursor: 'grabbing',
			handle: 'div.<?= ZBX_STYLE_DRAG_ICON ?>',
			tolerance: 'pointer',
			opacity: 0.6,
			helper: function(e, ui) {
				for (let td of ui.find('>td')) {
					let $td = $(td);
					$td.attr('width', $td.width())
				}

				// when dragging element on safari, it jumps out of the table
				if (SF) {
					// move back draggable element to proper position
					ui.css('left', (ui.offset().left - 2) + 'px');
				}

				return ui;
			},
			update: (e, ui) => {
				[...this.provision_groups_table.querySelectorAll('tbody tr')].forEach((row, i) => {
					row.querySelector('[name^="provision_groups"][name$="[sortorder]"]').value = i + 1;
				});
			},
			stop: function(e, ui) {
				ui.item.find('>td').removeAttr('width');
				ui.item.removeAttr('style');
			},
			start: function(e, ui) {
				$(ui.placeholder).height($(ui.helper).height());
			}
		});
	}

	showPasswordField(e) {
		const form_field = e.target.parentNode;
		const password_field = form_field.querySelector('[name="bind_password"][type="password"]');
		const password_var = form_field.querySelector('[name="bind_password"][type="hidden"]');

		password_field.style.display = '';
		password_field.disabled = false;

		if (password_var !== null) {
			form_field.removeChild(password_var);
		}
		form_field.removeChild(e.target);
	}

	openTestPopup() {
		const fields = this.preprocessFormFields(getFormFields(this.form));
		const test_overlay = PopUp('popup.ldap.test.edit', fields, {dialogueid: 'ldap_test_edit'});
		test_overlay.xhr.then(() => this.overlay.unsetLoading());
	}

	submit() {
		this.removePopupMessages();
		this.overlay.setLoading();

		const fields = this.preprocessFormFields(getFormFields(this.form));
		const curl = new Curl(this.form.getAttribute('action'), false);

		fetch(curl.getUrl(), {
			method: 'POST',
			headers: {'Content-Type': 'application/json'},
			body: JSON.stringify(fields)
		})
			.then((response) => response.json())
			.then((response) => {
				if ('error' in response) {
					throw {error: response.error};
				}

				overlayDialogueDestroy(this.overlay.dialogueid);

				this.dialogue.dispatchEvent(new CustomEvent('dialogue.submit', {detail: response.body}));
			})
			.catch((exception) => {
				let title;
				let messages = [];

				if (typeof exception === 'object' && 'error' in exception) {
					title = exception.error.title;
					messages = exception.error.messages;
				}
				else {
					title = <?= json_encode(_('Unexpected server error.')) ?>;
				}

				const message_box = makeMessageBox('bad', messages, title, true, true)[0];

				this.form.parentNode.insertBefore(message_box, this.form);
			})
			.finally(() => {
				this.overlay.unsetLoading();
			});
	}

	removePopupMessages() {
		for (const el of this.form.parentNode.children) {
			if (el.matches('.msg-good, .msg-bad, .msg-warning')) {
				el.parentNode.removeChild(el);
			}
		}
	}

	preprocessFormFields(fields) {
		this.trimFields(fields);

		if (fields.advanced_configuration != 1) {
			delete fields.start_tls;
			delete fields.search_filter;
		}

		if (fields.provision_status != 1) {
			delete fields.group_basedn;
			delete fields.group_name;
			delete fields.group_member;
			delete fields.group_filter;
			delete fields.group_membership;
			delete fields.user_username;
			delete fields.user_lastname;
			delete fields.provision_groups;
			delete fields.provision_media;
		}

		if (fields.userdirectoryid == null) {
			delete fields.userdirectoryid;
		}

		delete fields.advanced_configuration;

		return fields;
	}

	trimFields(fields) {
		const fields_to_trim = ['name', 'host', 'base_dn', 'bind_dn', 'search_attribute', 'search_filter',
			'description', 'group_basedn', 'group_name', 'group_member', 'group_filter', 'group_membership',
			'user_username', 'user_lastname'
		];

		for (const field of fields_to_trim) {
			if (field in fields) {
				fields[field] = fields[field].trim();
			}
		}
	}

	editProvisionGroup(row = null) {
		let popup_params;
		let row_index = 0;
		let sortorder;
		let status = <?= GROUP_MAPPING_FALLBACK_OFF ?>;
		const fallback_row = this.dialogue.querySelector('[data-row_fallback]');

		if (row === null) {
			while (this.provision_groups_table.querySelector(`[data-row_index="${row_index}"]`) !== null) {
				row_index++;
			}

			popup_params = {
				add_group: 1,
				is_fallback: <?= GROUP_MAPPING_REGULAR ?>
			};
			sortorder = parseInt(fallback_row.querySelector(`[name^="provision_groups"][name$="[sortorder]"]`).value);
		}
		else {
			row_index = row.dataset.row_index;
			let is_fallback = row.querySelector(`[name="provision_groups[${row_index}][is_fallback]"`).value;
			let user_groups = row.querySelectorAll(`[name="provision_groups[${row_index}][user_groups][][usrgrpid]"`);
			sortorder = parseInt(row.querySelector(`[name="provision_groups[${row_index}][sortorder]"`).value);

			popup_params = {
				usrgrpid: [...user_groups].map(usrgrp => usrgrp.value),
				roleid: row.querySelector(`[name="provision_groups[${row_index}][roleid]"`).value,
				is_fallback: is_fallback
			};
			if (is_fallback == <?= GROUP_MAPPING_REGULAR ?>) {
				popup_params.name = row.querySelector(`[name="provision_groups[${row_index}][name]"`).value;
			}
			else {
				status = row.querySelector(`[name="provision_groups[${row_index}][fallback_status]"`).value;
			}
		}

		popup_params.idp_type = <?= IDP_TYPE_LDAP ?>;

		const overlay = PopUp('popup.usergroupmapping.edit', popup_params, {dialogueid: 'user_group_edit'});

		overlay.$dialogue[0].addEventListener('dialogue.submit', (e) => {
			const group = {...e.detail, ...{row_index: row_index, fallback_status: status, sortorder: sortorder}};

			if (row === null) {
				fallback_row.parentNode.insertBefore(this._renderProvisionGroupRow(group), fallback_row);
				fallback_row.querySelector(`[name^="provision_groups"][name$="[sortorder]"`).value = sortorder + 1;
			}
			else {
				row.replaceWith(this._renderProvisionGroupRow(group));
			}
		});
	}

	editProvisionMediaType(row = null) {
		let popup_params;
		let row_index = 0;

		if (row === null) {
			while (document.querySelector(`#ldap-media-type-mapping-table [data-row_index="${row_index}"]`) !== null) {
				row_index++;
			}

			popup_params = {
				add_media_type_mapping: 1
			};
		}
		else {
			row_index = row.dataset.row_index;

			popup_params = {
				name: row.querySelector('[name="provision_media[' + row_index + '][name]"]').value,
				attribute: row.querySelector('[name="provision_media[' + row_index + '][attribute]"]').value,
				mediatypeid: row.querySelector('[name="provision_media[' + row_index + '][mediatypeid]"]').value
			};
		}

		const overlay = PopUp('popup.mediatypemapping.edit', popup_params, {dialogueid: 'media_type_mapping_edit'});

		overlay.$dialogue[0].addEventListener('dialogue.submit', (e) => {
			const mapping = {...e.detail, ...{row_index: row_index}};

			if (row === null) {
				this.dialogue
					.querySelector('#ldap-media-type-mapping-table tbody')
					.appendChild(this._renderProvisionMediaRow(mapping));
			}
			else {
				row.replaceWith(this._renderProvisionMediaRow(mapping));
			}
		});
	}

	_renderProvisionGroups(groups) {
		for (const key in groups) {
			let order = parseInt(key) + 1
			this.provision_groups_table
				.querySelector('tbody')
				.appendChild(this._renderProvisionGroupRow({...groups[key], ...{row_index: key, sortorder: order}}));
		}
	}

	_renderProvisionGroupRow(group) {
		const template = document.createElement('template');
		const template_row = group.is_fallback == <?= GROUP_MAPPING_FALLBACK ?>
			? new Template(this._templateProvisionFallbackGroupRow())
			: new Template(this._templateProvisionGroupRow());
		const attributes = {
			user_group_names: Object.values(group.user_groups).map(user_group => user_group.name).join(', ')
		};

		if (group.is_fallback == <?= GROUP_MAPPING_FALLBACK ?>) {
			if (group.fallback_status == <?= GROUP_MAPPING_FALLBACK_ON ?>) {
				attributes.action_label = '<?= _('Enabled') ?>';
				attributes.action_class = 'js-enabled <?= ZBX_STYLE_GREEN ?>';
			}
			else {
				attributes.action_label = '<?= _('Disabled') ?>';
				attributes.action_class = 'js-disabled <?= ZBX_STYLE_RED ?>';
			}
		}

		template.innerHTML = template_row.evaluate({...group, ...attributes}).trim();
		const row = template.content.firstChild;

		for (const user of Object.values(group.user_groups)) {
			const input = document.createElement('input');
			input.name = 'provision_groups[' + group.row_index + '][user_groups][][usrgrpid]';
			input.value = user.usrgrpid;
			input.type = 'hidden';

			row.appendChild(input);
		}

		return row;
	}

	_templateProvisionFallbackGroupRow() {
		return `
			<tr data-row_index="#{row_index}" data-row_fallback>
				<td></td>
				<td>
					<a href="javascript:void(0);" class="wordwrap js-edit"><?= _('Fallback group') ?></a>
					<input type="hidden" name="provision_groups[#{row_index}][roleid]" value="#{roleid}">
					<input type="hidden" name="provision_groups[#{row_index}][is_fallback]" value="<?= GROUP_MAPPING_FALLBACK ?>">
					<input type="hidden" name="provision_groups[#{row_index}][fallback_status]" value="#{fallback_status}">
					<input type="hidden" name="provision_groups[#{row_index}][sortorder]" value="#{sortorder}">
				</td>
				<td class="wordbreak">#{user_group_names}</td>
				<td class="wordbreak">#{role_name}</td>
				<td>
					<button type="button" class="<?= ZBX_STYLE_BTN_LINK ?> #{action_class}">#{action_label}</button>
				</td>
			</tr>
		`;
	}

	_templateProvisionGroupRow() {
		return `
			<tr data-row_index="#{row_index}" class="sortable">
				<td class="td-drag-icon">
					<div class="drag-icon ui-sortable-handle"></div>
				</td>
				<td>
					<a href="javascript:void(0);" class="wordwrap js-edit">#{name}</a>
					<input type="hidden" name="provision_groups[#{row_index}][name]" value="#{name}">
					<input type="hidden" name="provision_groups[#{row_index}][roleid]" value="#{roleid}">
					<input type="hidden" name="provision_groups[#{row_index}][is_fallback]" value="<?= GROUP_MAPPING_REGULAR ?>">
					<input type="hidden" name="provision_groups[#{row_index}][sortorder]" value="#{sortorder}">
				</td>
				<td class="wordbreak">#{user_group_names}</td>
				<td class="wordbreak">#{role_name}</td>
				<td>
					<button type="button" class="<?= ZBX_STYLE_BTN_LINK ?> js-remove"><?= _('Remove') ?></button>
				</td>
			</tr>
		`;
	}

	_renderProvisionMedia(provision_media) {
		for (const key in provision_media) {
			document
				.querySelector('#ldap-media-type-mapping-table tbody')
				.appendChild(this._renderProvisionMediaRow({...provision_media[key], ...{row_index: key}}));
		}
	}

	_renderProvisionMediaRow(provision_media) {
		const template_ldap_media_mapping_row = new Template(`
			<tr data-row_index="#{row_index}">
				<td>
					<a href="javascript:void(0);" class="wordwrap js-edit">#{name}</a>
					<input type="hidden" name="provision_media[#{row_index}][name]" value="#{name}">
					<input type="hidden" name="provision_media[#{row_index}][mediatype_name]" value="#{mediatype_name}">
					<input type="hidden" name="provision_media[#{row_index}][mediatypeid]" value="#{mediatypeid}">
					<input type="hidden" name="provision_media[#{row_index}][attribute]" value="#{attribute}">
				</td>
				<td class="wordbreak">#{mediatype_name}</td>
				<td class="wordbreak">#{attribute}</td>
				<td>
					<button type="button" class="<?= ZBX_STYLE_BTN_LINK ?> js-remove"><?= _('Remove') ?></button>
				</td>
			</tr>`);

		const template = document.createElement('template');
		template.innerHTML = template_ldap_media_mapping_row.evaluate(provision_media).trim();

		return template.content.firstChild;
	}
}();
