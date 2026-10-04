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
})();
</script>
<%in p/footer.cgi %>
