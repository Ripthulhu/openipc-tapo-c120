#!/usr/bin/haserl
<%in p/common.cgi %>
<% page_title="C120 Lights" %>
<%in p/header.cgi %>

<fieldset class="border-0 p-0 mb-4" id="c120-lights" disabled>
	<legend class="h5">Camera light</legend>
	<div class="mj-seg flex-wrap" role="group" aria-label="Camera light">
		<% for mode in off 850 940 both white; do
			case "$mode" in
				off) label="Off" ;;
				850) label="850 nm" ;;
				940) label="940 nm" ;;
				both) label="850 + 940 nm" ;;
				white) label="White" ;;
			esac %>
		<input type="radio" class="mj-seg-in" name="c120-light" id="c120-<%= $mode %>" value="<%= $mode %>" autocomplete="off">
		<label class="mj-seg-lbl" for="c120-<%= $mode %>"><%= $label %></label>
		<% done %>
	</div>
</fieldset>
<p id="c120-light-status" class="text-body-secondary" role="status">Reading light state...</p>
<p id="c120-light-error" class="text-danger" role="alert" hidden></p>
<form id="c120-motion-form" class="mt-4 mb-4">
	<fieldset id="c120-motion-fields" class="border-0 p-0" disabled>
		<legend class="h5">Motion floodlight</legend>
		<div class="form-check form-switch mb-3">
			<input id="c120-motion-enabled" class="form-check-input" type="checkbox">
			<label class="form-check-label" for="c120-motion-enabled">Night-only motion trigger</label>
		</div>
		<label class="form-label" for="c120-motion-trigger-seconds">Motion delay</label>
		<div class="input-group mb-3" style="max-width:16rem">
			<input id="c120-motion-trigger-seconds" class="form-control" type="number" min="0" max="600" step="1" value="3" required>
			<span class="input-group-text">seconds</span>
		</div>
		<label class="form-label" for="c120-motion-seconds">Duration after motion</label>
		<div class="input-group mb-3" style="max-width:16rem">
			<input id="c120-motion-seconds" class="form-control" type="number" min="1" max="600" step="1" value="30" required>
			<span class="input-group-text">seconds</span>
		</div>
		<button class="btn btn-primary" type="submit">Save Changes</button>
	</fieldset>
	<p id="c120-motion-status" class="text-body-secondary mt-3 mb-0" role="status">Reading timer state...</p>
	<p id="c120-motion-error" class="text-danger mt-2 mb-0" role="alert" hidden></p>
</form>
<p><a href="c120-light-pins.cgi">Camera light GPIO pins</a></p>
<p><a href="camera.cgi?tab=nightMode">Day / Night settings</a></p>

<script>
(() => {
	const group = document.getElementById('c120-lights');
	const status = document.getElementById('c120-light-status');
	const error = document.getElementById('c120-light-error');
	const choices = [...group.querySelectorAll('input')];
	let current;
	async function update(mode) {
		group.disabled = true;
		error.hidden = true;
		try {
			const response = await fetch('c120-light.cgi?mode=' + encodeURIComponent(mode), {cache: 'no-store'});
			if (!response.ok) throw new Error('Light request failed');
			const data = await response.json();
			if (!choices.some(choice => choice.value === data.mode)) throw new Error('Invalid light state');
			current = data.mode;
			status.textContent = 'Current: ' + document.querySelector('label[for="c120-' + current + '"]').textContent;
		} catch (failure) {
			error.textContent = failure.message + '. Reload the page to try again.';
			error.hidden = false;
			status.textContent = current ? status.textContent : 'Light state unavailable';
		} finally {
			choices.forEach(choice => { choice.checked = choice.value === current; });
			group.disabled = !current;
		}
	}
	group.addEventListener('change', event => update(event.target.value));
	update('status');

	const form = document.getElementById('c120-motion-form');
	const fields = document.getElementById('c120-motion-fields');
	const enabled = document.getElementById('c120-motion-enabled');
	const seconds = document.getElementById('c120-motion-seconds');
	const triggerSeconds = document.getElementById('c120-motion-trigger-seconds');
	const timerStatus = document.getElementById('c120-motion-status');
	const timerError = document.getElementById('c120-motion-error');
	let busy = false;
	let loaded = false;
	async function timerRequest(save = false) {
		if (busy) return;
		busy = true;
		if (save) fields.disabled = true;
		try {
			const options = {cache: 'no-store'};
			if (save) Object.assign(options, {
				method: 'POST',
				body: new URLSearchParams({enabled: enabled.checked ? '1' : '0', seconds: seconds.value,
					triggerSeconds: triggerSeconds.value})
			});
			const response = await fetch('c120-motion-light.cgi', options);
			const data = await response.json();
			if (!response.ok) throw new Error(data.error || 'Could not read timer settings');
			if (!loaded || save) {
				enabled.checked = data.enabled;
				seconds.value = data.seconds;
				triggerSeconds.value = data.triggerSeconds;
				loaded = true;
			}
			timerStatus.textContent = !data.running ? 'Camera helper stopped' : !data.available ?
				'Floodlight GPIO is assigned to another control' : data.active ?
				'Floodlight on: ' + data.remaining + ' s remaining' : data.enabled ? 'Armed' : 'Disabled';
			timerError.hidden = true;
		} catch (failure) {
			timerError.textContent = failure.message + ' Try again.';
			timerError.hidden = false;
		} finally {
			fields.disabled = !loaded;
			busy = false;
		}
	}
	form.addEventListener('submit', event => {
		event.preventDefault();
		if (form.reportValidity()) timerRequest(true);
	});
	timerRequest();
	const timerPoll = setInterval(() => { if (!document.hidden) timerRequest(); }, 3000);
	window.addEventListener('pagehide', () => clearInterval(timerPoll), {once: true});
})();
</script>
<%in p/footer.cgi %>
