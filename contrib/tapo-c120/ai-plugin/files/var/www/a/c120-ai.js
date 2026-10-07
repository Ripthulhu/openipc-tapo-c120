(() => {
    'use strict';
    const el = id => document.getElementById('ai-' + id);
    const fields = el('fields'), preview = el('image');
    let token, loaded = false, busy = false, savePending = false, imageBusy = false, detections = [];
    let models = [], selectedModel = 'stock', activeModel = '', previousModel = 'stock', writable = false, uploading = false, cancelUpload = false;
    const confidence = () => { el('confidence-value').textContent = el('confidence').value + '%'; };
    el('confidence').addEventListener('input', confidence);
    const gain = () => { el('sound-gain-value').textContent = '+'+el('sound-gain').value+' dB'; };
    el('sound-gain').addEventListener('input', gain);
    el('form').addEventListener('input', () => { el('saved').textContent = ''; });
    const sounds = ['bark', 'meow', 'cry', 'glass'];
    const names = {person: 'Person', pet: 'Pet (cat/dog)', vehicle: 'Vehicle', bird: 'Bird', bark: 'Barking', meow: 'Meowing', cry: 'Baby crying', glass: 'Glass breaking'};
    const categories = Object.keys(names);
    for (const group of ['notify', 'record']) for (const category of categories) {
        const col = document.createElement('div'), check = document.createElement('div'), input = document.createElement('input'), title = document.createElement('label');
        col.className = 'col-6'; check.className = 'form-check'; input.className = 'form-check-input'; input.type = 'checkbox';
        input.id = 'ai-'+group+'-'+category; title.htmlFor = input.id; title.className = 'form-check-label'; title.textContent = names[category];
        check.append(input,title); col.append(check); el(group+'-categories').append(col);
    }
    const label = o => names[o.label] || (o.label === 'unverified' ? 'Unverified class ' + o.classId : o.label);
    function modelControls() {
        const model = models.find(m => m.id === el('model').value);
        el('nms-field').hidden = !model?.categories.includes('bird');
        for (const group of ['notify','record']) for (const category of ['person','pet','vehicle','bird']) {
            const input = el(group+'-'+category);
            input.disabled = !model?.categories.includes(category);
        }
    }
    el('nms').addEventListener('input', () => { el('nms-value').textContent = el('nms').value+'%'; });
    el('model').addEventListener('change', () => {
        const model = models.find(m => m.id === el('model').value);
        if (model) { el('confidence').value = Math.round(model.confidence*100); el('nms').value = Math.round(model.nms*100); confidence(); el('nms-value').textContent=el('nms').value+'%'; }
        modelControls();
    });
    function renderModels(data) {
        if (!data.models) return;
        models = data.models.items; writable = data.models.writable;
        activeModel = data.activeModel || ''; selectedModel = data.config.model; previousModel = data.previousModel || 'stock';
        const desired = loaded ? el('model').value : selectedModel;
        const ids = models.map(m => m.id);
        if (!ids.includes(selectedModel)) ids.push(selectedModel);
        if (!ids.includes(desired)) ids.push(desired);
        const options = ids.map(id => { const option=document.createElement('option'); option.value=id; option.textContent=models.find(m=>m.id===id)?.name || id+' (unavailable)'; return option; });
        el('model').replaceChildren(...options); el('model').value=desired;
        el('model-storage').textContent = data.models.storage + (writable ? '' : data.models.storage === 'No mounted SD card' ? '' : ' (read-only)');
        el('model-active').textContent = activeModel ? 'Running: '+(data.activeModelName || activeModel)+(data.modelFallback?' (fallback)':'') : 'No visual model running';
        el('model-error').textContent = data.modelError || ''; el('model-error').hidden = !data.modelError;
        const rows = [...models,...(data.models.uploads || []).map(id=>({id,name:id+' (unfinished upload)',categories:[],location:'SD card',upload:true}))].map(m => {
            const row=document.createElement('tr');
            for (const value of [m.name+(m.active?' · running':'')+(m.selected?' · selected':''),m.categories.map(c=>names[c] || c).join(', '),m.location]) {
                const cell=document.createElement('td'); cell.textContent=value; row.append(cell);
            }
            const actions=document.createElement('td');
            if (m.id!=='stock') {
                const button=document.createElement('button'); button.type='button'; button.className='btn btn-outline-danger btn-sm'; button.textContent=m.upload?'Cancel':'Remove';
                button.disabled=uploading || !writable || !m.upload && [selectedModel,activeModel,previousModel].includes(m.id);
                button.addEventListener('click',async()=>{
                    if (!window.confirm('Remove '+m.name+' from the SD card?')) return;
                    button.disabled=true;
                    try { await modelAction({modelAction:m.upload?'cancel':'remove',id:m.id}); await request(); }
                    catch (e) { el('error').textContent=e.message; el('error').hidden=false; }
                }); actions.append(button);
            }
            row.append(actions); return row;
        });
        el('model-list').replaceChildren(...rows); el('upload-fields').disabled=!token || !writable || uploading;
        modelControls();
    }
    function render(data) {
        el('status').textContent = data.status;
        detections = data.objects || [];
        el('objects').textContent = detections.length ? detections.map(o => label(o) + ' ' + Math.round(o.confidence * 100) + '%').join(' · ') : 'No current detections';
        el('boxes').replaceChildren();
        for (const o of detections) {
            if (!Array.isArray(o.box) || o.box.length !== 4 || !o.box.every(Number.isFinite)) continue;
            const box = document.createElement('div'), title = document.createElement('span');
            box.className = 'ai-box';
            const [x, y, w, h] = o.box;
            Object.assign(box.style, {left: x*100+'%', top: y*100+'%', width: w*100+'%', height: h*100+'%'});
            title.textContent = label(o) + ' ' + Math.round(o.confidence * 100) + '%';
            box.append(title); el('boxes').append(box);
        }
        const rows = (data.events || []).slice().reverse().map(e => {
            const row = document.createElement('tr');
            for (const value of [new Date(e.time*1000).toLocaleTimeString(), label(e), Math.round(e.confidence*100)+'%']) {
                const cell = document.createElement('td'); cell.textContent = value; row.append(cell);
            }
            return row;
        });
        if (!rows.length) {
            const row = document.createElement('tr'), cell = document.createElement('td');
            cell.colSpan = 3; cell.className = 'text-body-secondary'; cell.textContent = 'No detections yet'; row.append(cell); rows.push(row);
        }
        el('events').replaceChildren(...rows);
        el('stats').textContent = data.running ? Math.round(data.inferenceMs)+' ms inference · '+data.frames+' frames analyzed' : '';
        el('sound-status').textContent = data.soundStatus || 'Unavailable';
        el('sounds').textContent = (data.sounds || []).length ? data.sounds.map(s => label(s)+' '+Math.round(s.confidence*100)+'%').join(' · ') : 'No current sound detections';
        el('sound-stats').textContent = data.soundRunning ? Number(data.soundInferenceMs).toFixed(1)+' ms inference · '+data.soundFrames+' windows analyzed' : '';
        el('notify-status').textContent = data.notifications?.status || 'Service stopped';
        el('record-status').textContent = data.recording?.status || 'Service stopped';
        el('notify-test').disabled = !data.config.notifications.enabled || !data.running && !data.soundRunning;
    }
    async function request(save = false) {
        if (busy) { if (save) savePending = true; return; }
        busy = true;
        if (save) fields.disabled = true;
        try {
            const options = {cache: 'no-store', signal: AbortSignal.timeout(5000)};
            if (save) Object.assign(options, {method: 'POST', headers: {'Content-Type': 'application/json'}, body: JSON.stringify({
                csrf: token, model: el('model').value, nms: +el('nms').value/100,
                enabled: el('enabled').checked, confidence: +el('confidence').value/100,
                intervalMs: +el('interval').value, motionRegions: el('regions').checked,
                soundEnabled: el('sound-enabled').checked, soundSensitivity: +el('sound-sensitivity').value,
                soundGainDb: +el('sound-gain').value,
                soundClasses: sounds.filter(s => el(s).checked),
                notifications: {enabled: el('notify-enabled').checked, format: el('notify-format').value,
                    url: el('notify-url').value.trim(), token: el('notify-token').value,
                    cooldownSeconds: +el('notify-cooldown').value, categories: categories.filter(s => el('notify-'+s).checked)},
                recording: {enabled: el('record-enabled').checked, seconds: +el('record-seconds').value,
                    categories: categories.filter(s => el('record-'+s).checked)}
            })});
            const response = await fetch('c120-ai-api.cgi', options), data = await response.json();
            if (!response.ok) throw new Error(data.error || 'AI request failed');
            token = data.csrf;
            renderModels(data);
            if (!loaded || save) {
                el('enabled').checked = data.config.enabled; el('confidence').value = Math.round(data.config.confidence*100);
                el('interval').value = data.config.intervalMs; el('regions').checked = data.config.motionRegions;
                el('model').value = data.config.model; el('nms').value = Math.round(data.config.nms*100);
                el('nms-value').textContent = el('nms').value+'%'; modelControls();
                el('sound-enabled').checked = data.config.soundEnabled;
                el('sound-sensitivity').value = data.config.soundSensitivity;
                el('sound-gain').value = data.config.soundGainDb; gain();
                for (const sound of sounds) el(sound).checked = data.config.soundClasses.includes(sound);
                const n = data.config.notifications, r = data.config.recording;
                el('notify-enabled').checked = n.enabled; el('notify-format').value = n.format;
                el('notify-url').value = n.url; el('notify-token').value = n.token; el('notify-cooldown').value = n.cooldownSeconds;
                el('record-enabled').checked = r.enabled; el('record-seconds').value = r.seconds;
                for (const category of categories) {
                    el('notify-'+category).checked = n.categories.includes(category);
                    el('record-'+category).checked = r.categories.includes(category);
                }
                confidence(); loaded = true;
            }
            render(data); el('error').hidden = true;
            if (save) el('saved').textContent = 'Saved';
        } catch (e) {
            el('error').textContent = e.message; el('error').hidden = false;
            el('status').textContent = 'Connection unavailable'; el('boxes').replaceChildren();
            el('objects').textContent = 'Detection state unavailable';
            el('sound-status').textContent = 'Connection unavailable'; el('sounds').textContent = 'Detection state unavailable';
            el('notify-status').textContent = el('record-status').textContent = 'Connection unavailable';
            el('notify-test').disabled = true;
        } finally {
            busy = false; fields.disabled = !loaded;
            if (savePending) { savePending = false; request(true); }
        }
    }
    function snapshot() {
        if (document.hidden || imageBusy) return;
        imageBusy = true; preview.src = '/image.jpg?_ai=' + Date.now();
    }
    preview.onload = () => { imageBusy = false; el('image-error').hidden = true; };
    preview.onerror = () => { imageBusy = false; el('image-error').hidden = false; };
    el('form').addEventListener('submit', e => {
        e.preventDefault();
        if (el('sound-enabled').checked && !sounds.some(s => el(s).checked)) {
            el('error').textContent = 'Select at least one sound.'; el('error').hidden = false; return;
        }
        for (const group of ['notify','record']) if (el(group+'-enabled').checked && !categories.some(s => el(group+'-'+s).checked)) {
            el('error').textContent = 'Select at least one category for '+(group === 'notify' ? 'notifications.' : 'recording.'); el('error').hidden = false; return;
        }
        if (el('notify-enabled').checked && !el('notify-url').value.trim()) {
            el('error').textContent = 'Enter a notification URL.'; el('error').hidden = false; return;
        }
        request(true);
    });
    el('notify-test').addEventListener('click', async () => {
        el('notify-test').disabled = true;
        try {
            const response = await fetch('c120-ai-api.cgi', {method: 'POST', headers: {'Content-Type':'application/json'},
                signal: AbortSignal.timeout(5000), body: JSON.stringify({csrf:token,testNotification:true})});
            const data = await response.json(); if (!response.ok) throw new Error(data.error || 'Test failed');
            el('notify-status').textContent = data.status;
        } catch (e) { el('error').textContent = e.message; el('error').hidden = false; }
    });
    async function modelAction(payload) {
        const response=await fetch('c120-ai-api.cgi',{method:'POST',headers:{'Content-Type':'application/json'},
            body:JSON.stringify({csrf:token,...payload}),signal:AbortSignal.timeout(15000)});
        const data=await response.json(); if (!response.ok) throw new Error(data.error || 'Model request failed');
        return data;
    }
    el('upload-cancel').addEventListener('click',()=>{ cancelUpload=true; });
    el('upload-form').addEventListener('submit',async e=>{
        e.preventDefault(); if (uploading || !token || !writable) return;
        let id, started=false;
        uploading=true; cancelUpload=false; el('upload-fields').disabled=true;
        el('upload-cancel').hidden=false; el('upload-cancel').disabled=false;
        el('upload-progress').hidden=false; el('upload-progress').value=0;
        try {
            const profileFile=el('upload-profile').files[0], file=el('upload-model').files[0];
            if (!profileFile || profileFile.size>4096 || !file || file.size>8*1024*1024) throw new Error('Select a small profile and a model up to 8 MiB.');
            const profile=JSON.parse(await profileFile.text()); id=profile.id;
            if (profile.modelBytes!==file.size) throw new Error('Model size does not match its profile.');
            await modelAction({modelAction:'start',id,profile}); started=true;
            for (let offset=0;offset<file.size;offset+=3072) {
                if (cancelUpload) throw new Error('Upload cancelled.');
                const bytes=new Uint8Array(await file.slice(offset,offset+3072).arrayBuffer());
                let binary=''; for (const byte of bytes) binary+=String.fromCharCode(byte);
                await modelAction({modelAction:'chunk',id,offset,data:btoa(binary)});
                el('upload-progress').value=Math.round((offset+bytes.length)*100/file.size);
                el('upload-status').textContent='Uploading: '+el('upload-progress').value+'%';
            }
            if (cancelUpload) throw new Error('Upload cancelled.');
            el('upload-status').textContent='Checking model...';
            await modelAction({modelAction:'finish',id}); started=false;
            el('upload-status').textContent='Model uploaded'; el('upload-form').reset();
        } catch (failure) {
            el('upload-status').textContent=failure.message;
            if (started) try { await modelAction({modelAction:'cancel',id}); } catch { el('upload-status').textContent+=' Upload cleanup failed; retry when the card is available.'; }
        } finally {
            uploading=false; el('upload-cancel').hidden=true;
            el('upload-fields').disabled=!writable; await request();
        }
    });
    async function poll() {
        if (!document.hidden) { await request(); snapshot(); }
        window.setTimeout(poll, 1000);
    }
    poll();
})();
