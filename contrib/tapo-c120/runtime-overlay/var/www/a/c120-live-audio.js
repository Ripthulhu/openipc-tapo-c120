/* Keep native WebRTC talkback; this only exposes HTTPS and the camera's output gain. */
window.addEventListener("load", async function () {
    const audio = document.getElementById("mj-audio-ctl");
    const talk = document.getElementById("mj-talk-ctl");
    if (!audio || !talk || document.getElementById("c120-speaker")) return;
    const cfg = await mjConfig();
    if (!cfg.audio || !cfg.system) return;

    const speaker = document.createElement("span");
    speaker.id = "c120-speaker";
    speaker.className = "mj-hud mj-tog-wrap";
    const label = document.createElement("label");
    label.htmlFor = "c120-speaker-volume";
    label.className = "mj-tog";
    const icon = audio.querySelector("svg").cloneNode(true);
    icon.querySelector(".mj-ic-off").remove();
    icon.querySelector(".mj-ic-on").removeAttribute("class");
    label.append(icon, "Speaker");
    const volume = document.createElement("input");
    volume.id = "c120-speaker-volume";
    volume.type = "range";
    volume.min = "0";
    volume.max = "100";
    volume.step = "1";
    volume.className = "mj-vol";
    volume.setAttribute("aria-label", "Camera speaker volume");
    const value = document.createElement("output");
    value.htmlFor = volume.id;
    value.className = "mj-tog";
    const status = document.createElement("span");
    status.id = "c120-speaker-status";
    status.className = "mj-hud";
    status.setAttribute("role", "status");
    status.hidden = true;
    speaker.append(label, volume, value, status);
    audio.after(speaker);

    let saved = Number(cfg.audio.outputVolume);
    const enabled = cfg.audio.enabled === true && cfg.audio.outputEnabled === true;
    function show(level) {
        volume.value = String(level);
        value.textContent = String(level);
        volume.setAttribute("aria-valuetext", level === 0 ? "Muted" : level + " percent");
        volume.title = "Camera speaker volume: " + level + "% (0 mutes)";
    }
    show(saved);
    volume.disabled = !enabled;
    if (!enabled) speaker.title = "Enable audio and speaker output in Audio settings.";
    volume.addEventListener("input", function () { show(Number(volume.value)); });
    volume.addEventListener("change", async function () {
        const level = Number(volume.value);
        if (!Number.isInteger(level) || level < 0 || level > 100 || volume.disabled) return;
        if (level === saved) return;
        volume.disabled = true;
        speaker.setAttribute("aria-busy", "true");
        status.textContent = "Saving speaker";
        status.hidden = false;
        const abort = new AbortController();
        const timeout = setTimeout(function () { abort.abort(); }, 10000);
        let persisted = false;
        try {
            const response = await apiFetch("/api/v1/config", {
                method: "POST", signal: abort.signal,
                headers: {"Content-Type": "application/json"},
                body: JSON.stringify({audio: {outputVolume: level}})
            });
            if (!response.ok) throw new Error("save");
            const check = await apiFetch("/api/v1/config.json", {signal: abort.signal, cache: "no-store"});
            if (!check.ok || (await check.json()).audio.outputVolume !== level) throw new Error("verify");
            saved = level;
            persisted = true;
            // SigmaStar applies output gain when its native audio pipeline is rebuilt.
            const apply = await apiFetch("/cgi-bin/j/mj-apply.cgi", {method: "POST", signal: abort.signal});
            if (!apply.ok) throw new Error("apply");
            status.hidden = true;
        } catch (error) {
            status.textContent = persisted ? "Saved; restart Majestic to apply" : "Save failed; try again";
        } finally {
            clearTimeout(timeout);
            show(saved);
            volume.disabled = !enabled;
            speaker.removeAttribute("aria-busy");
        }
    });

    if (location.protocol === "http:" && enabled && typeof RTCPeerConnection !== "undefined") {
        let url;
        try {
            const response = await apiFetch("/a/c120-live-audio.json", {cache: "no-store"});
            if (response.ok) {
                const settings = await response.json();
                const candidate = new URL(settings.secureLiveUrl);
                if (candidate.protocol === "https:" && !candidate.username && !candidate.password) url = candidate;
            }
        } catch (error) { /* A proxy address is optional on other installations. */ }
        if (!url && cfg.system.httpsCertificate) {
            url = new URL(location.href);
            url.protocol = "https:";
            url.port = String(cfg.system.httpsPort || 443);
        }
        const secure = document.createElement("span");
        secure.id = "c120-secure-talk";
        secure.className = "mj-hud mj-tog-wrap";
        const link = document.createElement(url ? "a" : "button");
        link.className = "mj-tog mj-tog-amber";
        link.title = url ? "Open secure Live to use your microphone" : "An HTTPS connection is required for Talk";
        if (url) link.href = url.href;
        else { link.type = "button"; link.disabled = true; }
        link.append(talk.querySelector("svg").cloneNode(true), "Talk");
        secure.append(link);
        speaker.after(secure);
    }
});
