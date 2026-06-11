#!/bin/sh

fw_setenv sensor sc430ai
fw_setenv srcfg '0 1 1 0 1 1'
fw_setenv wlandev rtl8188fu-ssc377-tapo-c120
fw_setenv bootdelay 1

cli -s .system.logLevel info
cli -s .network.eth0.enabled false

cli -s .isp.antiFlicker disabled

cli -s .nightMode.colorToGray true
cli -s .nightMode.irCutPin1 81
cli -s .nightMode.irCutSingleInvert true
cli -s .nightMode.backlightPin 12
cli -s .nightMode.lightMonitor false
cli -s .nightMode.lightSensorInvert false
cli -s .nightMode.monitorDelay 0

cli -s .video0.enabled true
cli -s .video0.codec h264
cli -s .video0.size 2432x1376
cli -s .video0.fps 60
cli -s .video0.bitrate 10000
cli -s .video0.rcMode cbr
cli -s .video0.gopSize 60
cli -s .video1.enabled false
cli -s .jpeg.enabled false

cli -s .audio.enabled true
cli -s .audio.codec opus
cli -s .audio.srate 8000
cli -s .audio.volume 50
cli -s .audio.speakerPin 43
cli -s .audio.outputEnabled false
cli -s .audio.outputVolume 30

cli -s .motionDetect.enabled false
cli -s .motionDetect.visualize false
cli -s .motionDetect.debug false
cli -s .motionDetect.sensitivity 3
cli -s .records.enabled false

# The stock crontab is empty on the C120 profile; leave crond disabled to save RAM.
[ -x /etc/init.d/S60crond ] && /etc/init.d/S60crond stop >/dev/null 2>&1 || true
chmod 0644 /etc/init.d/S60crond 2>/dev/null || true
