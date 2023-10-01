<?php
/*
** Zabbix
** Copyright (C) 2001-2023 Zabbix SIA
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


class CControllerDashboardPrint extends CController {

	protected function init() {
		$this->disableCsrfValidation();
	}

	protected function checkInput(): bool {
		$fields = [
			'dashboardid' =>	'required|db dashboard.dashboardid',
			'from' =>			'range_time',
			'to' =>				'range_time'
		];

		$ret = $this->validateInput($fields) && $this->validateTimeSelectorPeriod();

		if (!$ret) {
			$this->setResponse(new CControllerResponseFatal());
		}

		return $ret;
	}

	protected function checkPermissions(): bool {
		return $this->checkAccess(CRoleHelper::UI_MONITORING_DASHBOARD);
	}

	protected function doAction() {
		[$dashboard, $page_sizes, $error] = $this->getDashboard();

		if ($error !== null) {
			$this->setResponse(new CControllerResponseData(['error' => $error]));

			return;
		}

		$time_selector_options = [
			'profileIdx' => 'web.dashboard.filter',
			'profileIdx2' => $dashboard['dashboardid'] ?? 0,
			'from' => $this->hasInput('from') ? $this->getInput('from') : null,
			'to' => $this->hasInput('to') ? $this->getInput('to') : null
		];

		$data = [
			'dashboard' => $dashboard,
			'page_sizes' => $page_sizes,
			'widget_defaults' => APP::ModuleManager()->getWidgetsDefaults(),
			'dashboard_time_period' => getTimeSelectorPeriod($time_selector_options)
		];

		$response = new CControllerResponseData($data);
		$response->setTitle(_('Dashboard'));
		$this->setResponse($response);
	}

	/**
	 * Get dashboard data from API.
	 *
	 * @return array
	 */
	private function getDashboard(): array {
		$dashboard = null;
		$error = null;
		$page_sizes = [];

		$db_dashboards = API::Dashboard()->get([
			'output' => ['dashboardid', 'name', 'display_period'],
			'selectPages' => ['dashboard_pageid', 'name', 'display_period', 'widgets'],
			'dashboardids' => [$this->getInput('dashboardid')]
		]);

		if ($db_dashboards) {
			$dashboard = $db_dashboards[0];
			$dashboard['pages'] = CDashboardHelper::preparePages(
				CDashboardHelper::prepareWidgetsAndForms($dashboard['pages'], null),
				$dashboard['pages'],
				true
			);

			foreach($dashboard['pages'] as $index => $page) {
				$max = 0;

				foreach($page['widgets'] as $widget) {
					$max = max($max, $widget['pos']['y'] + $widget['pos']['height']);
				}

				$page_sizes[$index] = $max * DASHBOARD_ROW_HEIGHT + 12;
			}
		}
		else {
			$error = _('No permissions to referred object or it does not exist!');
		}

		return [$dashboard, $page_sizes, $error];
	}
}
