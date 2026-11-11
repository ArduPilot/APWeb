/*
  helper functions for MAVLink commands
*/

var MAV_CMD_DO_MOTOR_TEST = 209;
var MAV_CMD_START_RX_PAIR = 500;
var MAV_CMD_PREFLIGHT_REBOOT_SHUTDOWN = 246;
var MAV_CMD_REQUEST_AUTOPILOT_CAPABILITIES = 520;
var MAV_CMD_REQUEST_MESSAGE = 512;
var MAVLINK_MSG_ID_AUTOPILOT_VERSION = 148;
var MAV_CMD_PREFLIGHT_CALIBRATION = 241;
var MAV_CMD_ACCELCAL_VEHICLE_POS = 42429;
var MAV_CMD_DO_START_MAG_CAL = 42424;
var MAV_CMD_FIXED_MAG_CAL = 42004;
var MAV_CMD_FIXED_MAG_CAL_FIELD = 42005;

var MAV_SYS_STATUS_SENSOR_RC_RECEIVER = 0x10000;

var MAV_SEVERITY_EMERGENCY = 0;
var MAV_SEVERITY_ALERT = 1;
var MAV_SEVERITY_CRITICAL = 2;
var MAV_SEVERITY_ERROR = 3;
var MAV_SEVERITY_WARNING = 4;
var MAV_SEVERITY_NOTICE = 5;
var MAV_SEVERITY_INFO = 6;
var MAV_SEVERITY_DEBUG = 7;

// scaling and numdigits table for variables, by regular expression
const scaling = { 'RAW_IMU:.acc' : [9.81 * 0.001, 3],
                  'GPS_RAW_INT:l..' : [1.0e-7, 7],
                  'GPS_RAW_INT:alt' : [1.0e-3, 2],
                  'GLOBAL_POSITION_INT:l..' : [1.0e-7, 7],
                  'GLOBAL_POSITION_INT:relative' : [1.0e-3, 2],
                  'ATTITUDE:time_boot_ms' : [1.0e-3, 0],
                  'ATTITUDE:roll' : [180/Math.PI, 1],
                  'ATTITUDE:pitch' : [180/Math.PI, 1],
                  'ATTITUDE:yaw' : [180/Math.PI, 1],
                  'SCALED_PRESSURE:temperature' : [0.01, 1],
                  'GPS2_RAW:vel' : [0.01, 2],
                  'GPS2_RAW:ep.' : [0.01, 2],
                  'SYS_STATUS:voltage_battery' : [0.001, 2]
                }
var scaling_re = {}
for (var r in scaling) {
    scaling_re[r] = new RegExp(r);
}

// return scaled variable
function scale_variable(varname, value) {
    var scale = 1.0;
    var fixed = -1;
    for (var r in scaling) {
        if (scaling_re[r].test(varname)) {
            scale = scaling[r][0];
            fixed = scaling[r][1];
            break;
        }
    }
    value = value * scale;
    if (fixed >= 0) {
        value = value.toFixed(fixed);
    }
    return value;
}

var mavlink_msg_types = []

/*
  fill in all divs of form MAVLINK:MSGNAME:field at refresh_ms() rate
*/
function fill_mavlink_ids(options={}) {
    function again() {
        setTimeout(function() { fill_mavlink_ids(options); }, refresh_ms());
    }
    if (mavlink_msg_types.length == 0) {
        /*
          work out what mavlink messages we need to fetch by looking
          through all names
        */
        if ('extra_msgs' in options) {
            mavlink_msg_types = options.extra_msgs;
        }
        var divs = document.querySelectorAll('[name^="MAVLINK:"]');
        var numdivs = divs.length;
        for (var i = 0; i < numdivs; i++) {
            var divname = divs[i].attributes.name.value;
            var x = divname.split(":");
            var msg_name = x[1];
            if (!mavlink_msg_types.includes(msg_name)) {
                mavlink_msg_types.push(msg_name);
            }
        }
    }
    var xhr = createCORSRequest("POST", drone_url + "/ajax/command.json");
    var form = new FormData();
    form.append('command1', 'mavlink_message(' + mavlink_msg_types.join() + ')');
    xhr.onload = function() {
        again();
        var mavlink;
        var text = xhr.responseText;
        text = text.replace(/(\r\n|\n|\r)/gm," ");
        try {
            mavlink = JSON.parse(text);
        } catch(e) {
            console.log(e);
            return;
        }
        var chart_lines = {}
        if ('chart_lines' in options) {
            chart_lines = options.chart_lines;
        }
        for (var i=0, len=mavlink_msg_types.length; i<len; i++) {
            var msg = mavlink_msg_types[i];
            for (var v in mavlink[msg]) {
                var fname = "MAVLINK:" + msg + ":" + v;
                var elements = document.getElementsByName(fname);
                if (elements.length > 0 || fname in chart_lines) {
                    var value = scale_variable(fname, mavlink[msg][v]);
                }
                for (var j=0; j<elements.length; j++) {
                    elements[j].innerHTML = value;
                }
                if (fname in chart_lines) {
                    for (var j=0; j<chart_lines[fname].length; j++) {
                        chart_lines[fname][j].append(new Date().getTime(), value);
                    }
                }
            }
        }
        if ('callback_fn' in options) {
            options.callback_fn(mavlink);
        }
    }
    xhr.onerror = function() {
        console.log("fill_mavlink_ids command error");
        again();
    };
    xhr.timeout = 3000;
    xhr.ontimeout = function() {
        console.log("fill_mavlink_ids command timeout");
        again();
    }
    xhr.send(form);

    if (activeTab == "Camera") {
        var now = new Date().getTime();
        if (now - last_refresh_ms > 2*refresh_ms()) {
            // cope with bad image downloads
            refresh_camera();
        }
    }
}


/*
  fill variables in a page from json
*/
function page_fill_json_value(json) {
    for (var v in json) {
        var element = document.getElementById(v);
        if (element) {
            element.value = json[v];
        }
    }
}

/*
  fill html in a page from json
*/
function page_fill_json_html(json) {
    for (var v in json) {
        var element = document.getElementById(v);
        if (element) {
            element.innerHTML = json[v];
        }
    }
}

/*
  send a command function
*/
function command_send(command, options={}) {
    var args = Array.prototype.slice.call(arguments);
    var xhr = createCORSRequest("POST", drone_url + "/ajax/command.json");
    var form = new FormData();
    if (options['onload']) {
        xhr.onload = function() {
            options.onload(xhr.responseText);
        }
    }
    if (options['onerror']) {
        xhr.timeout = 3000;
        if (options['timeout']) {
            xhr.timeout = options['timeout'];
        }
        xhr.onerror = function() {
            options.onerror();
        }
        xhr.ontimeout = function() {
            options.onerror();
        }
    }
    if (options['fillid']) {
        xhr.onload = function() {
            var fillid = options['fillid'];
            var element = document.getElementById(options.fillid);
            if (element) {
                element.innerHTML = xhr.responseText;
            }
        }
    }
    if (options['filljson'] == true) {
        xhr.onload = function() {
            try {
                var fill = JSON.parse(xhr.responseText);
            } catch(e) {
                console.log(e);
                return;
            }
            page_fill_json_value(fill);
        }
    }
    if (options['extra_args']) {
        var extra = options['extra_args'];
        for (var k in extra) {
            form.append(k, extra[k]);
        }
    }
    if (command.constructor === Array) {
        // allow for multiple commands
        for (var i=0; i<command.length; i++) {
            var cnum = i+1;
            form.append("command" + cnum, command[i]);            
        }
    } else {
        form.append("command1", command);
    }
    xhr.send(form);
}

/*
  send a mavlink command
*/
function mavlink_message_send() {
    var args = Array.prototype.slice.call(arguments);
    var mavcmd = "mavlink_message_send(" + args.join() + ")";
    command_send(mavcmd);
}

/*
  send a mavlink command long
*/
function mavlink_command_long_send() {
    var args = Array.prototype.slice.call(arguments);
    var mavcmd = "mavlink_message_send(COMMAND_LONG,0,0," + args.join() + ")";
    command_send(mavcmd);
}


/*
  send a STATUSTEXT for logging
*/
function mavlink_statustext(severity, text) {
    var mavcmd = "mavlink_message_send(STATUSTEXT," + severity + "," + text + ")";
    command_send(mavcmd);
}


/*
  send a mavlink command long with a callback giving the result code
*/
function mavlink_command_long_callback(args, callback, timeout_ms) {
    var mavcmd = "mavlink_message_send(COMMAND_LONG,0,0," + args.join() + ")";
    var extra_args = { "command_ack" : args[0], "command_ack_timeout" : timeout_ms };
    function command_error() {
        callback("-1");
    }
    command_send(mavcmd, { "extra_args" : extra_args,
                           "onload" : callback,
                           "onerror" : command_error,
                           "timeout" : timeout_ms });
}

/*
  fetch a URL, calling a callback
*/
function ajax_get_callback(url, callback) {
    var xhr = createCORSRequest("GET", url);
    xhr.onload = function() {
        callback(xhr.responseText);
    }
    xhr.send();
}

/*
  fetch a URL, calling a callback for binary data
*/
function ajax_get_callback_binary(url, callback) {
    var xhr = createCORSRequest("GET", url);
    xhr.onload = function() {
        console.log("got response length " + xhr.response.byteLength);
        callback(xhr.response);
    }
    xhr.responseType = "arraybuffer";
    xhr.send();
}

/*
  poll a URL, calling a callback
*/
function ajax_poll(url, callback, refresh_ms=1000) {
    function again() {
        setTimeout(function() { ajax_poll(url, callback, refresh_ms); }, refresh_ms);
    }
    var xhr = createCORSRequest("GET", url);
    xhr.onload = function() {
        if (callback(xhr.responseText)) {
            again();
        }
    }
    xhr.onerror = function() {
        again();
    }
    xhr.timeout = 3000;
    xhr.ontimeout = function() {
        again();
    }
    xhr.send();
}


/*
  poll a json file and fill document IDs at the given rate
*/
function ajax_json_poll(url, callback, refresh_ms=1000) {
    function do_callback(responseText) {
        try {
            var json = JSON.parse(responseText);
            return callback(json);
        } catch(e) {
            return true;
        }
        /* on bad json keep going */
        return true;
    }
    ajax_poll(url, do_callback, refresh_ms);
}

/*
  poll a json file and fill document IDs at the given rate
*/
function ajax_json_poll_fill(url, refresh_ms=1000) {
    function callback(json) {
        page_fill_json_html(json);
        return true;
    }
    ajax_json_poll(url, callback, refresh_ms);
}


/*
  set some parameters from a hash
*/
function mavlink_set_params(param_hash) {
    var commands = [];
    for (var k in param_hash) {
        var value = param_hash[k];
        commands.push("mavlink_message_send(PARAM_SET,0,0," + k + "," + value + ")");
    }
    command_send(commands);
}

/*
  set a message in a div by id, with given color
*/
function set_message_color(id, color, message) {
    var element = document.getElementById(id);
    if (element) {
        element.innerHTML = '<b style="color:' + color + '">' + message + '</b>';
    }
}

/*
  append a message in a div by id, with given color
*/
function append_message_color(id, color, message) {
    var element = document.getElementById(id);
    if (element) {
        element.innerHTML += '<br><b style="color:' + color + '">' + message + '</b>';
    }
    mavlink_statustext(MAV_SEVERITY_INFO, message);
}

/*
  get utc time in seconds
*/
function get_utc_sec() {
    var d = new Date();
    var dsec = d.getTime() / 1000;
    return dsec;
}

/*
  set the date on the sonix board
*/
function set_sonix_date()
{
    var d = new Date();
    var dsec = get_utc_sec();
    var tz_offset = -d.getTimezoneOffset() * 60;
    // toFixed() rounds to the nearest second already: adding 0.5 too
    // made the time we give the board up to a second fast
    d = dsec.toFixed(0);
    var cmd = "set_time_utc(" + d + "," + tz_offset + ")";
    command_send(cmd);
}

// set date now and every 20s
set_sonix_date();
setInterval(set_sonix_date, 20000);

/*
  show in the corner of every page whether the drone is answering,
  and whether it's hearing from the flight controller.  The dot
  pulses on each answer; once the drone or the flight controller has
  been quiet for a while it says how long ago it was last seen.

  The drone keeps the last HEARTBEAT it received on its flight
  controller link, so the HEARTBEAT's _age is the time since it last
  heard from that link.  On the SkyViper only the flight controller
  sends on it (it doesn't route a GCS's HEARTBEATs back to us); with
  the Linux build fed by a router that forwards other components'
  HEARTBEATs this could stay fresh after the flight controller stops.
*/
var link_poll_ms = 1000;
var link_stale_ms = 3000;
var link_drone_seen_ms = 0;     // our clock when the drone last answered
var link_fc_age_ms = null;      // HEARTBEAT age when it answered; null if none yet
var link_indicator = null;

function link_indicator_create() {
    var div = document.createElement("div");
    div.style.cssText = "position:fixed; top:6px; right:8px; z-index:1000; padding:2px 8px;" +
        "border-radius:10px; background:rgba(255,255,255,0.85); font:12px sans-serif;" +
        "color:#333; display:flex; align-items:center; gap:6px; pointer-events:none";
    var dot = document.createElement("span");
    dot.style.cssText = "display:inline-block; width:10px; height:10px; border-radius:50%; background:#999";
    var text = document.createElement("span");
    div.appendChild(dot);
    div.appendChild(text);
    document.body.appendChild(div);
    link_indicator = { div: div, dot: dot, text: text };
    link_indicator_update();
}

function link_seconds(ms) {
    return Math.floor(ms / 1000) + " s";
}

function link_indicator_update() {
    if (link_indicator == null) {
        return;
    }
    var now = Date.now();
    var colour, text;
    if (link_drone_seen_ms == 0) {
        colour = "#999";
        text = "Drone not seen";
    } else if (now - link_drone_seen_ms > link_stale_ms) {
        colour = "#c33";
        text = "Drone last seen " + link_seconds(now - link_drone_seen_ms) + " ago";
    } else if (link_fc_age_ms == null) {
        colour = "#e90";
        text = "Flight controller not seen";
    } else if (link_fc_age_ms > link_stale_ms) {
        colour = "#e90";
        text = "Flight controller last seen " + link_seconds(link_fc_age_ms) + " ago";
    } else {
        colour = "#2a2";
        text = "";
    }
    link_indicator.dot.style.background = colour;
    link_indicator.text.textContent = text;
    link_indicator.text.style.display = text ? "" : "none";
}

function link_poll() {
    var xhr = createCORSRequest("POST", drone_url + "/ajax/command.json");
    var form = new FormData();
    form.append("command1", "mavlink_message(HEARTBEAT)");
    var done = false;
    function again() {
        if (!done) {
            done = true;
            setTimeout(link_poll, link_poll_ms);
        }
    }
    xhr.onload = function() {
        again();
        var json;
        try {
            json = JSON.parse(xhr.responseText.replace(/(\r\n|\n|\r)/gm, " "));
        } catch(e) {
            return;
        }
        link_drone_seen_ms = Date.now();
        link_fc_age_ms = ("HEARTBEAT" in json) ? json.HEARTBEAT._age : null;
        link_indicator_update();
        if (link_indicator && link_indicator.dot.animate) {
            link_indicator.dot.animate([{ transform: "scale(1.6)" }, { transform: "scale(1)" }], 300);
        }
    };
    xhr.onerror = again;
    xhr.timeout = 2000;
    xhr.ontimeout = again;
    xhr.send(form);
}

if (document.readyState == "loading") {
    document.addEventListener("DOMContentLoaded", link_indicator_create);
} else {
    link_indicator_create();
}
setInterval(link_indicator_update, 1000);
link_poll();
