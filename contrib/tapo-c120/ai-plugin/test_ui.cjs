// Dependency-free checks for full refreshes, lightweight polling and unsaved edits.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');
const nodes = new Map(), timers = [], calls = [], events = {};
function node() {
    return {value:'', checked:false, disabled:false, hidden:false, children:[], listeners:{},
        append(...children) { this.children.push(...children); },
        replaceChildren(...children) { this.children = children; },
        addEventListener(name, cb) { this.listeners[name] = cb; }};
}
function el(id) { if (!nodes.has(id)) nodes.set(id,node()); return nodes.get(id); }
const config = {model:'stock', nms:.45, enabled:true, confidence:.6, intervalMs:500,
    motionRegions:true, soundEnabled:true, soundSensitivity:1, soundGainDb:12,
    soundClasses:['bark'], notifications:{enabled:true, format:'webhook', url:'https://example.test/',
        token:'fixture', cooldownSeconds:60, categories:['pet']},
    recording:{enabled:true, seconds:30, categories:['person']}};
const status = {csrf:'f'.repeat(64), status:'Detecting', activeModel:'stock', previousModel:'stock',
    running:true, frames:10, inferenceMs:50, objects:[], events:[], soundRunning:true, soundFrames:20,
    soundInferenceMs:1, notifications:{status:'Ready'}, recording:{status:'Waiting'}};
let time = 1000, failure = false;
const document = {hidden:false, getElementById:el, createElement:node,
    addEventListener(name,cb) { events[name] = cb; }};
const context = {document, console, AbortSignal, Date:class extends Date { static now() {return time;} },
    window:{setTimeout(cb) {timers.push(cb);}},
    async fetch(url, options) {
        calls.push({url, options});
        if (failure) throw Error('Offline');
        const full = !url.includes('?view=status');
        const data = {...status, ...(full ? {config:structuredClone(config), models:{writable:true,
            storage:'SD card', items:[{id:'stock', name:'Stock', categories:['person','pet','vehicle'],
                confidence:.6, nms:.45, location:'Flash'}]}} : {})};
        return {ok:true, async json() {return data;}};
    }};
const settle = () => new Promise(resolve => setImmediate(resolve));
async function poll() { assert(timers.length); await timers.shift()(); await settle(); }
(async () => {
    vm.runInNewContext(fs.readFileSync(__dirname+'/files/var/www/a/c120-ai.js','utf8'),context);
    await settle();
    assert.equal(calls[0].url,'c120-ai-api.cgi');
    const rows = el('ai-model-list').children;
    el('ai-confidence').value = 77;
    await poll();
    assert.equal(calls.at(-1).url,'c120-ai-api.cgi?view=status');
    assert.equal(el('ai-model-list').children,rows);
    assert.equal(el('ai-confidence').value,77);
    assert.equal(el('ai-notify-test').disabled,false);
    time += 60001; await poll();
    assert.equal(calls.at(-1).url,'c120-ai-api.cgi');
    assert.equal(el('ai-confidence').value,77);
    const count=calls.length;
    document.hidden=true; await poll(); assert.equal(calls.length,count);
    document.hidden=false; events.visibilitychange(); await poll();
    assert.equal(calls.at(-1).url,'c120-ai-api.cgi');
    status.activeModel='bird'; await poll(); await poll();
    assert.equal(calls.at(-1).url,'c120-ai-api.cgi');
    failure=true; await poll();
    assert.equal(el('ai-status').textContent,'Connection unavailable');
    assert.equal(el('ai-notify-test').disabled,true);
    failure=false;
    el('ai-form').listeners.submit({preventDefault(){}}); await settle();
    assert.equal(calls.at(-1).options.method,'POST');
    assert.equal(JSON.parse(calls.at(-1).options.body).confidence,.77);
    assert.equal(el('ai-saved').textContent,'Saved');
    console.log('PASS: lightweight status polls, minute/focus/model refresh, hidden pause, unsaved edits, errors and full saves');
})().catch(error=>{console.error(error); process.exitCode=1;});
