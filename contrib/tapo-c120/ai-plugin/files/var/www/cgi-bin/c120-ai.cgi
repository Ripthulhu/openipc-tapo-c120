#!/usr/bin/haserl
<%in p/common.cgi %>
<% page_title="AI Detection" %>
<%in p/header.cgi %>

<div class="d-flex justify-content-between align-items-center gap-3 mb-3 flex-wrap">
    <h2 class="h5 mb-0">Object detection</h2>
    <span id="ai-status" class="text-body-secondary" role="status">Connecting...</span>
</div>
<div id="ai-preview">
    <img id="ai-image" src="/image.jpg" alt="Camera preview" width="1280" height="720">
    <div id="ai-boxes" aria-hidden="true"></div>
    <span id="ai-image-error" hidden>Preview unavailable</span>
</div>
<p id="ai-objects" class="mt-2 text-body-secondary" aria-live="polite">No current detections</p>
<p id="ai-error" class="text-danger" role="alert" hidden></p>
<div class="row g-4 mt-1">
    <form id="ai-form" class="col-12 col-lg-5">
        <fieldset id="ai-fields" class="border-0 p-0" disabled>
            <legend class="h5">Detection settings</legend>
            <label class="form-label" for="ai-model">Detector model</label>
            <select class="form-select mb-2" id="ai-model"><option value="stock">Stock detector</option></select>
            <p class="mb-2"><a href="#ai-model-library">Model library</a></p>
            <p id="ai-model-active" class="text-body-secondary mb-2" role="status"></p>
            <p id="ai-model-error" class="text-warning mb-3" role="status" hidden></p>
            <div class="form-check form-switch mb-3">
                <input class="form-check-input" type="checkbox" id="ai-enabled">
                <label class="form-check-label" for="ai-enabled">Object detection</label>
            </div>
            <label class="form-label" for="ai-confidence">Minimum confidence <output id="ai-confidence-value">60%</output></label>
            <input class="form-range mb-3" type="range" id="ai-confidence" min="25" max="99" step="1" value="60">
            <div id="ai-nms-field" hidden>
                <label class="form-label" for="ai-nms">NMS overlap <output id="ai-nms-value">45%</output></label>
                <input class="form-range mb-3" type="range" id="ai-nms" min="5" max="95" step="1" value="45">
            </div>
            <label class="form-label" for="ai-interval">Analysis interval</label>
            <div class="input-group mb-3" style="max-width:16rem">
                <input class="form-control" type="number" id="ai-interval" min="500" max="5000" step="1" value="500" required>
                <span class="input-group-text">ms</span>
            </div>
            <div class="form-check form-switch mb-3">
                <input class="form-check-input" type="checkbox" id="ai-regions" checked>
                <label class="form-check-label" for="ai-regions">Use motion regions</label>
            </div>
            <p><a href="camera.cgi?tab=motionDetect">Motion regions</a></p>
            <h3 class="h5 mt-4">Sound recognition <span class="small text-body-secondary">Experimental</span></h3>
            <div class="form-check form-switch mb-3">
                <input class="form-check-input" type="checkbox" id="ai-sound-enabled">
                <label class="form-check-label" for="ai-sound-enabled">Sound detection</label>
            </div>
            <label class="form-label" for="ai-sound-sensitivity">Sensitivity</label>
            <select class="form-select mb-3" id="ai-sound-sensitivity" style="max-width:16rem">
                <option value="0">Low</option><option value="1" selected>Normal</option><option value="2">High</option>
            </select>
            <label class="form-label" for="ai-sound-gain">Analysis gain <output id="ai-sound-gain-value">+12 dB</output></label>
            <input class="form-range mb-3" type="range" id="ai-sound-gain" min="0" max="24" step="1" value="12">
            <fieldset class="border-0 p-0 mb-4">
                <legend class="fs-6">Sounds</legend>
                <div class="row g-2">
                    <div class="col-6"><div class="form-check"><input class="form-check-input" type="checkbox" id="ai-bark" checked><label class="form-check-label" for="ai-bark">Barking</label></div></div>
                    <div class="col-6"><div class="form-check"><input class="form-check-input" type="checkbox" id="ai-meow" checked><label class="form-check-label" for="ai-meow">Meowing</label></div></div>
                    <div class="col-6"><div class="form-check"><input class="form-check-input" type="checkbox" id="ai-cry" checked><label class="form-check-label" for="ai-cry">Baby crying</label></div></div>
                    <div class="col-6"><div class="form-check"><input class="form-check-input" type="checkbox" id="ai-glass" checked><label class="form-check-label" for="ai-glass">Glass breaking</label></div></div>
                </div>
            </fieldset>
            <h3 class="h5 mt-4">Notifications</h3>
            <div class="form-check form-switch mb-3"><input class="form-check-input" type="checkbox" id="ai-notify-enabled"><label class="form-check-label" for="ai-notify-enabled">Send notifications</label></div>
            <label class="form-label" for="ai-notify-format">Destination</label>
            <select class="form-select mb-3" id="ai-notify-format"><option value="webhook">Home Assistant / webhook</option><option value="ntfy">ntfy</option></select>
            <label class="form-label" for="ai-notify-url">URL</label>
            <input class="form-control mb-3" type="url" id="ai-notify-url" maxlength="512" autocomplete="off">
            <label class="form-label" for="ai-notify-token">Bearer token <span class="text-body-secondary">Optional</span></label>
            <input class="form-control mb-3" type="password" id="ai-notify-token" maxlength="512" autocomplete="new-password">
            <label class="form-label" for="ai-notify-cooldown">Minimum time between alerts per category</label>
            <div class="input-group mb-3" style="max-width:16rem"><input class="form-control" type="number" id="ai-notify-cooldown" min="10" max="3600" value="60" required><span class="input-group-text">s</span></div>
            <fieldset class="border-0 p-0 mb-3"><legend class="fs-6">Notify On</legend><div class="row g-2" id="ai-notify-categories"></div></fieldset>
            <button class="btn btn-outline-secondary btn-sm" id="ai-notify-test" type="button">Send Test</button>
            <span id="ai-notify-status" class="ms-2 text-body-secondary" role="status"></span>
            <h3 class="h5 mt-4">AI Recording</h3>
            <div class="form-check form-switch mb-3"><input class="form-check-input" type="checkbox" id="ai-record-enabled"><label class="form-check-label" for="ai-record-enabled">Record selected detections</label></div>
            <label class="form-label" for="ai-record-seconds">Time after the last detection</label>
            <div class="input-group mb-3" style="max-width:16rem"><input class="form-control" type="number" id="ai-record-seconds" min="1" max="600" value="30" required><span class="input-group-text">s</span></div>
            <fieldset class="border-0 p-0 mb-3"><legend class="fs-6">Record On</legend><div class="row g-2" id="ai-record-categories"></div></fieldset>
            <p id="ai-record-status" class="text-body-secondary" role="status"></p>
            <p><a href="camera.cgi?tab=records">Recording storage</a> &middot; <a href="recordings.cgi">Recordings</a> &middot; <a href="c120-lights.cgi">Floodlight triggers</a></p>
            <button class="btn btn-primary" type="submit">Save Changes</button>
            <span id="ai-saved" class="ms-2 text-body-secondary" role="status"></span>
        </fieldset>
    </form>
    <section id="ai-model-library" class="col-12 col-lg-7">
        <h3 class="h5">Model library</h3>
        <p id="ai-model-storage" class="text-body-secondary text-break" role="status">Reading storage...</p>
        <div class="table-responsive">
            <table class="table align-middle">
                <thead><tr><th scope="col">Model</th><th scope="col">Categories</th><th scope="col">Storage</th><th scope="col"><span class="visually-hidden">Actions</span></th></tr></thead>
                <tbody id="ai-model-list"></tbody>
            </table>
        </div>
        <form id="ai-upload-form" class="mb-4">
            <fieldset id="ai-upload-fields" class="border-0 p-0" disabled>
                <label class="form-label" for="ai-upload-profile">Model profile</label>
                <input type="file" class="form-control mb-3" id="ai-upload-profile" accept=".json" required>
                <label class="form-label" for="ai-upload-model">Compiled model</label>
                <input type="file" class="form-control mb-3" id="ai-upload-model" accept=".img" required>
                <button type="submit" class="btn btn-outline-secondary btn-sm">Upload Model</button>
            </fieldset>
            <button type="button" id="ai-upload-cancel" class="btn btn-outline-secondary btn-sm mt-2" hidden>Cancel Upload</button>
            <progress id="ai-upload-progress" class="w-100 mt-2" max="100" value="0" hidden aria-label="Model upload"></progress>
            <p id="ai-upload-status" class="text-body-secondary mt-2" role="status"></p>
        </form>
        <h3 class="h5">Recent detections</h3>
        <div class="table-responsive">
            <table class="table">
                <thead><tr><th scope="col">Time</th><th scope="col">Detection</th><th scope="col">Confidence</th></tr></thead>
                <tbody id="ai-events"><tr><td colspan="3" class="text-body-secondary">No detections yet</td></tr></tbody>
            </table>
        </div>
        <p id="ai-stats" class="text-body-secondary"></p>
        <h3 class="h5 mt-4">Sound recognition</h3>
        <p id="ai-sound-status" class="text-body-secondary" role="status">Connecting...</p>
        <p id="ai-sounds" aria-live="polite">No current sound detections</p>
        <p id="ai-sound-stats" class="text-body-secondary"></p>
        <p><a href="c120-ai-api.cgi">Detection API</a></p>
    </section>
</div>
<style>
#ai-preview{position:relative;aspect-ratio:16/9;background:#17191c;overflow:hidden;border-radius:4px;max-width:100%}
#ai-image{width:100%;height:100%;display:block;object-fit:contain}
#ai-boxes{position:absolute;inset:0;pointer-events:none}
.ai-box{position:absolute;border:2px solid #4ce2a4}
.ai-box span{position:absolute;top:0;left:0;background:#17291f;color:#fff;font:12px/1.5 sans-serif;padding:1px 4px;white-space:nowrap;max-width:calc(100vw - 4rem);overflow:hidden}
#ai-image-error{position:absolute;inset:0;align-content:center;text-align:center;color:#bec3ca}
#ai-confidence-value{margin-left:.5rem}
#ai-model-library .table-responsive{overflow-x:auto;max-width:100%}
#ai-model-library td{overflow-wrap:anywhere}
#ai-model-library td:last-child{width:1%;white-space:nowrap;overflow-wrap:normal}
#ai-model-library button{white-space:nowrap}
#ai-model-library .visually-hidden{position:absolute;width:1px;height:1px;margin:-1px;overflow:hidden;clip-path:inset(50%);white-space:nowrap}
</style>
<script src="/a/c120-ai.js?v=5"></script>
<%in p/footer.cgi %>
