/* Native browser/audio transport is untouched; exercise only the add-on's controls. */
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const source = fs.readFileSync(path.join(__dirname, 'runtime-overlay/var/www/a/c120-live-audio.js'), 'utf8');

class Element {
    constructor(tag) { this.tagName = tag; this.children = []; this.listeners = {}; this.attributes = {}; }
    append(...items) { this.children.push(...items); }
    after(item) { this.afterElement = item; }
    querySelector() { return new Element('svg'); }
    cloneNode() { return new Element(this.tagName); }
    remove() {}
    removeAttribute(name) { delete this.attributes[name]; }
    setAttribute(name, value) { this.attributes[name] = value; }
    addEventListener(name, fn) { this.listeners[name] = fn; }
}

async function check(protocol, enabled, mode = 'ok', secureLiveUrl = 'https://cam.example/cgi-bin/live.cgi') {
    const elements = [];
    const audio = new Element('span'), talk = new Element('span');
    const cfg = {audio: {enabled: true, outputEnabled: enabled, outputVolume: 80}, system: {}};
    const calls = [];
    let init;
    const document = {
        getElementById(id) {
            return id === 'mj-audio-ctl' ? audio : id === 'mj-talk-ctl' ? talk : elements.find(e => e.id === id);
        },
        createElement(tag) { const e = new Element(tag); elements.push(e); return e; }
    };
    const context = {window: {addEventListener(name, fn) { assert.equal(name, 'load'); init = fn; }},
        document, location: {protocol, href: protocol + '//camera/cgi-bin/live.cgi'},
        URL, AbortController, setTimeout, clearTimeout, RTCPeerConnection: function () {},
        mjConfig: async () => cfg,
        apiFetch: async (url, options = {}) => {
            calls.push([url, options]);
            if (url.endsWith('c120-live-audio.json')) return {ok: true, json: async () => ({secureLiveUrl})};
            if (url === '/api/v1/config') {
                if (mode === 'save-fail') return {ok: false};
                cfg.audio.outputVolume = JSON.parse(options.body).audio.outputVolume;
            }
            if (url === '/api/v1/config.json') return {ok: true, json: async () => cfg};
            return {ok: !(mode === 'apply-fail' && url === '/cgi-bin/j/mj-apply.cgi')};
        }
    };
    vm.runInNewContext(source, context);
    await init();
    const speaker = document.getElementById('c120-speaker');
    const volume = document.getElementById('c120-speaker-volume');
    assert.equal(speaker.className, 'mj-hud mj-tog-wrap');
    assert.equal(speaker.children[0].className, 'mj-tog');
    assert.equal(speaker.children[2].className, 'mj-tog');
    assert.equal(volume.className, 'mj-vol');
    assert.equal(volume.value, '80');
    assert.equal(volume.disabled, !enabled);
    assert(!source.includes('getUserMedia(') && !source.includes('new RTCPeerConnection'));
    if (protocol === 'http:' && enabled) {
        const link = document.getElementById('c120-secure-talk').children[0];
        assert.equal(link.tagName, secureLiveUrl.startsWith('https://cam.example/') ? 'a' : 'button');
        if (link.tagName === 'a') assert.equal(link.href, secureLiveUrl);
        else assert.equal(link.disabled, true);
    } else assert(!document.getElementById('c120-secure-talk'));
    calls.length = 0;
    volume.value = '93';
    volume.listeners.input();
    assert.equal(calls.length, 0, 'dragging must not reload the camera per pixel');
    await volume.listeners.change();
    if (enabled) {
        assert.equal(calls[0][0], '/api/v1/config');
        assert.deepEqual(JSON.parse(calls[0][1].body), {audio: {outputVolume: 93}});
        const status = document.getElementById('c120-speaker-status');
        assert.equal(volume.value, mode === 'save-fail' ? '80' : '93');
        assert.equal(status.hidden, mode === 'ok');
        assert.equal(volume.disabled, false);
        if (mode === 'apply-fail') assert(status.textContent.includes('restart Majestic'));
    } else assert.equal(calls.length, 0);
    calls.length = 0;
    volume.value = '101';
    await volume.listeners.change();
    assert.equal(calls.length, 0);
    await init();
    assert.equal(elements.filter(e => e.id === 'c120-speaker').length, 1);
}

(async () => {
    await check('https:', true);
    await check('https:', false);
    await check('http:', true);
    await check('http:', true, 'ok', 'http://cam.example/');
    await check('http:', true, 'ok', 'https://root:secret@cam.example/');
    await check('https:', true, 'save-fail');
    await check('https:', true, 'apply-fail');
    console.log('PASS Live audio: native styling, secure link, disabled output, scoped saves and failures');
})().catch(error => { console.error(error); process.exitCode = 1; });
