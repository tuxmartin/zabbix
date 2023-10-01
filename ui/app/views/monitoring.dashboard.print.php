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
** Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
**/


/**
 * @var CView $this
 * @var array $data
 */

const HEADER_TITLE_HEIGHT = 60;
const PAGE_TITLE_HEIGHT = 50;
const PAGE_WIDTH = 1940;
const PAGE_MARGIN = 10;


if (array_key_exists('error', $data)) {
	show_error_message($data['error']);

	return;
}

$this->addJsFile('flickerfreescreen.js');
$this->addJsFile('gtlc.js');
$this->addJsFile('leaflet.js');
$this->addJsFile('leaflet.markercluster.js');
$this->addJsFile('class.dashboard.js');
$this->addJsFile('class.dashboard.page.js');
$this->addJsFile('class.dashboard.widget.placeholder.js');
$this->addJsFile('class.geomaps.js');
$this->addJsFile('class.widget-base.js');
$this->addJsFile('class.widget.js');
$this->addJsFile('class.widget.inaccessible.js');
$this->addJsFile('class.widget.iterator.js');
$this->addJsFile('class.widget.misconfigured.js');
$this->addJsFile('class.widget.paste-placeholder.js');
$this->addJsFile('class.csvggraph.js');
$this->addJsFile('class.svg.canvas.js');
$this->addJsFile('class.svg.map.js');
$this->addJsFile('class.csvggauge.js');
$this->addJsFile('class.sortable.js');

$this->includeJsFile('monitoring.dashboard.print.js.php');

$this->addCssFile('assets/styles/vendors/Leaflet/Leaflet/leaflet.css');

$page_count = count($data['dashboard']['pages']);
$page_styles = '';

$header_title_tag = (new CTag('h1', true, $data['dashboard']['name']));

(new CTag('header', true, $header_title_tag))
	->addClass('header-title page_1')
	->show();

if ($page_count > 1) {
	foreach ($data['dashboard']['pages'] as $index => $dashboard_page) {
		$page_number = $index + 1;
		$page_name = 'page_' . $page_number;
		$page_height = $data['page_sizes'][$index] + PAGE_TITLE_HEIGHT + PAGE_MARGIN;

		if ($index === 0) {
			$page_height += HEADER_TITLE_HEIGHT;
		}

		$page_styles .= '@page '.$page_name.' { size: '.(PAGE_WIDTH).'px '.$page_height.'px; } ';
		$page_styles .= '.'.$page_name.' { page: '.$page_name.'; } ';

		(new CDiv())
			->addClass('dashboard-page page_'.$page_number)
			->addItem(
				new CTag('h1', true,
					$dashboard_page['name'] !== '' ? $dashboard_page['name'] : _s('Page %1$d', $page_number)
				)
			)
			->addItem(
				(new CDiv())->addClass(ZBX_STYLE_DASHBOARD_GRID)
			)
			->show();
	}
}
else {
	$page_name = 'page_1';
	$page_height = $data['page_sizes'][0] + HEADER_TITLE_HEIGHT + PAGE_MARGIN;

	$page_styles .= '@page '.$page_name.' { size: '.PAGE_WIDTH.'px '.$page_height.'px; } ';
	$page_styles .= '.'.$page_name.' { page: ' . $page_name.'; } ';

	(new CDiv())
		->addClass('dashboard-page page_1')
		->addItem(
			(new CDiv())->addClass(ZBX_STYLE_DASHBOARD_GRID)
		)
		->show();
}

(new CTag('style', true, $page_styles))->show();

(new CScriptTag('
	view.init('.json_encode([
		'dashboard' => $data['dashboard'],
		'widget_defaults' => $data['widget_defaults'],
		'dashboard_time_period' => $data['dashboard_time_period']
	]).');
'))
	->setOnDocumentReady()
	->show();
