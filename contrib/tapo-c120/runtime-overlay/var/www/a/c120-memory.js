(function () {
	const panel = document.getElementById('c120-memory-map');
	if (!panel || typeof mjMetricsSubscribe !== 'function') return;
	const bar = document.getElementById('c120-memory-bar');
	const legend = document.getElementById('c120-memory-legend');
	const total = document.getElementById('c120-memory-total');
	const note = document.getElementById('c120-memory-note');
	const MiB = 1048576;
	let metrics = null, map = null, nextRead = 0, reading = false;
	const mib = bytes => (bytes / MiB).toFixed(bytes % MiB ? 1 : 0) + ' MiB';
	const valid = n => Number.isSafeInteger(n) && n >= 0;

	function render() {
		if (!map || !metrics || !valid(metrics.memTotal) || !valid(metrics.memAvail)) return;
		const linuxTotal = metrics.memTotal;
		const linuxFree = Math.min(metrics.memAvail, linuxTotal);
		const other = map.physicalBytes - map.mediaBytes - linuxTotal;
		if (other < 0) { panel.hidden = true; return; }
		const mediaFree = valid(map.mediaFreeBytes) && map.mediaFreeBytes <= map.mediaBytes
			? map.mediaFreeBytes : null;
		const parts = mediaFree === null
			? [{ name: 'Media reserved', bytes: map.mediaBytes, ink: 'var(--st-c4, #8a5cd8)' }]
			: [
				{ name: 'Media in use', bytes: map.mediaBytes - mediaFree, ink: 'var(--st-c4, #8a5cd8)' },
				{ name: 'Media reserved, free', bytes: mediaFree, ink: 'repeating-linear-gradient(135deg, #b69add 0 4px, #d1bee9 4px 8px)' },
			];
		parts.push(
			{ name: 'Linux in use', bytes: linuxTotal - linuxFree, ink: 'var(--st-c1, #4c60d8)' },
			{ name: 'Linux available', bytes: linuxFree, ink: 'var(--st-c2, #0d9488)' },
			{ name: 'System reserved', bytes: other, ink: 'var(--bs-secondary-color, #7a7a8c)' },
		);
		bar.replaceChildren();
		legend.replaceChildren();
		for (const part of parts) {
			if (!part.bytes) continue;
			const segment = document.createElement('div');
			segment.className = 'seg';
			segment.style.width = (100 * part.bytes / map.physicalBytes) + '%';
			segment.style.background = part.ink;
			segment.title = part.name + ': ' + mib(part.bytes);
			bar.appendChild(segment);
			const label = document.createElement('span');
			const dot = document.createElement('i');
			dot.className = 'dot';
			dot.style.background = part.ink;
			label.append(dot, document.createTextNode(part.name + ' ' + mib(part.bytes)));
			legend.appendChild(label);
		}
		total.textContent = mib(map.physicalBytes) + ' physical';
		bar.setAttribute('aria-label', 'Physical RAM: ' + parts.map(p => p.name + ' ' + mib(p.bytes)).join(', '));
		note.textContent = mediaFree === null
			? mib(map.mediaBytes) + ' is reserved for the media stack; current use is unavailable.'
			: mib(map.mediaBytes) + ' is reserved for the media stack; ' +
				mib(map.mediaBytes - mediaFree) + ' is currently in use within that pool.';
		panel.hidden = false;
	}

	function readMap() {
		if (reading) return;
		reading = true;
		fetch('/cgi-bin/c120-memory.cgi', { cache: 'no-store' })
			.then(response => {
				if (!response.ok) throw new Error('Memory map unavailable');
				return response.json();
			})
			.then(value => {
				if (!valid(value.physicalBytes) || !valid(value.mediaBytes) ||
					value.physicalBytes === 0 || value.mediaBytes > value.physicalBytes)
					throw new Error('Invalid memory map');
				map = value;
				render();
			})
			.catch(() => { map = null; panel.hidden = true; })
			.finally(() => { reading = false; });
	}

	mjMetricsSubscribe(sample => {
		if (!sample.ok) return;
		metrics = sample;
		const now = Date.now();
		if (now >= nextRead) { nextRead = now + 10000; readMap(); }
		render();
	});
})();
