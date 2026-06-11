#!/usr/bin/haserl
<%in p/common.cgi %>

<% page_title="Camera Preview" %>
<%in p/header.cgi %>

<div class="row preview">
	<div class="col">
		<% preview %>
		<p class="small"><a href="mj-endpoints.cgi">Majestic Endpoints</a></p>
		<% if [ -x /usr/bin/c120-light-pinsd ]; then %>
			<p class="small"><a href="c120-light-pins.cgi">C120 Light Pins</a></p>
		<% fi %>
	</div>

	<div class="col-auto">
		<% if [ "$(get_night lightMonitor)" = "true" ]; then %>
			<p class="small"><a href="mj-settings.cgi?tab=nightMode">Light monitor active</a></p>
		<% fi %>

		<div class="d-grid gap-3">
			<input type="checkbox" class="btn-check" id="toggle-night">
			<label class="btn btn-primary" for="toggle-night">Night</label>

			<input type="checkbox" class="btn-check" id="toggle-ircut">
			<label class="btn btn-primary" for="toggle-ircut">IRcut</label>

			<input type="checkbox" class="btn-check" id="toggle-light">
			<label class="btn btn-primary" for="toggle-light">Light</label>

			<% if [ -x /usr/bin/c120-lamps ]; then %>
				<input type="radio" class="btn-check" name="c120-ir" id="c120-ir-off" autocomplete="off" checked>
				<label class="btn btn-outline-primary" for="c120-ir-off">IR Off</label>

				<input type="radio" class="btn-check" name="c120-ir" id="c120-ir-850" autocomplete="off">
				<label class="btn btn-outline-primary" for="c120-ir-850">850 nm</label>

				<input type="radio" class="btn-check" name="c120-ir" id="c120-ir-940" autocomplete="off">
				<label class="btn btn-outline-primary" for="c120-ir-940">940 nm</label>

				<input type="radio" class="btn-check" name="c120-ir" id="c120-ir-both" autocomplete="off">
				<label class="btn btn-outline-primary" for="c120-ir-both">850+940 nm</label>
			<% fi %>

			<% if [ -n "$ptz_support" ]; then %>
				<%in p/motor.cgi %>
			<% fi %>
		</div>
	</div>
</div>

<script>
<% echo "\$('#toggle-night').checked = $(get_metrics night_enabled);" %>
<% echo "\$('#toggle-ircut').checked = $(get_metrics ircut_enabled);" %>
<% echo "\$('#toggle-light').checked = $(get_metrics light_enabled);" %>

<% echo "\$('#toggle-night').disabled = $(get_night lightMonitor);" %>
<% echo "\$('#toggle-ircut').disabled = $(get_night lightMonitor) || !$(get_night irCutPin1);" %>
<% if [ -x /usr/bin/c120-lamps ]; then %>
$('#toggle-light').disabled = false;
<% else %>
<% echo "\$('#toggle-light').disabled = $(get_night lightMonitor) || !$(get_night backlightPin);" %>
<% fi %>

<% if [ -x /usr/bin/c120-lamps ]; then %>
function setC120LampState(data) {
	$('#toggle-light').checked = data.mode === 'white';
	$('#c120-ir-off').checked = !data.ir850 && !data.ir940;
	$('#c120-ir-850').checked = data.mode === '850';
	$('#c120-ir-940').checked = data.mode === '940';
	$('#c120-ir-both').checked = data.mode === 'both';
}

function c120Lamp(mode) {
	fetch('/cgi-bin/c120-light.cgi?mode=' + mode)
		.then(api => api.json())
		.then(setC120LampState);
}

c120Lamp('status');
<% fi %>

$("#toggle-night").addEventListener("click", ev => {
	fetch('/night/toggle').then(api => api.json()).then(data => {
		ev.target.checked = data;
		if (!$('#toggle-ircut').disabled) {
			$('#toggle-ircut').checked = data;
		}
		<% if [ ! -x /usr/bin/c120-lamps ]; then %>
		if (!$('#toggle-light').disabled) {
			$('#toggle-light').checked = data;
		}
		<% fi %>
	});
});

$("#toggle-ircut").addEventListener("click", ev => {
	fetch('/night/ircut').then(api => api.json()).then(data => {
		ev.target.checked = data;
	});
});

<% if [ -x /usr/bin/c120-lamps ]; then %>
$("#toggle-light").addEventListener("click", ev => c120Lamp(ev.target.checked ? 'white' : 'off'));
$("#c120-ir-off").addEventListener("click", ev => c120Lamp('off'));
$("#c120-ir-850").addEventListener("click", ev => c120Lamp('850'));
$("#c120-ir-940").addEventListener("click", ev => c120Lamp('940'));
$("#c120-ir-both").addEventListener("click", ev => c120Lamp('both'));
<% else %>
$("#toggle-light").addEventListener("click", ev => {
	fetch('/night/light').then(api => api.json()).then(data => {
		ev.target.checked = data;
	});
});
<% fi %>
</script>

<%in p/footer.cgi %>
