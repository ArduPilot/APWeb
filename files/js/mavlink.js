/*
  helper functions for MAVLink commands
*/

var MAV_CMD_DO_MOTOR_TEST = 209;
var MAV_CMD_START_RX_PAIR = 500;
var MAV_CMD_PREFLIGHT_REBOOT_SHUTDOWN = 246;
var MAV_CMD_REQUEST_AUTOPILOT_CAPABILITIES = 520;
var MAV_CMD_PREFLIGHT_CALIBRATION = 241;
var MAV_CMD_ACCELCAL_VEHICLE_POS = 42429;
var MAV_CMD_DO_START_MAG_CAL = 42424;

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

// from common.xml:
// var MAV_DATA_STREAM_ALL = 0;
const stream_ids = {
    MAV_DATA_STREAM_RAW_SENSORS : 1,
    MAV_DATA_STREAM_EXTENDED_STATUS : 2,
    MAV_DATA_STREAM_RC_CHANNELS : 3,
    MAV_DATA_STREAM_RAW_CONTROLLER : 4,
    MAV_DATA_STREAM_POSITION : 6,
    MAV_DATA_STREAM_EXTRA1 : 10,
    MAV_DATA_STREAM_EXTRA2 : 11,
    MAV_DATA_STREAM_EXTRA3 : 12
}

const stream_for_msg = {
    'RAW_IMU' : "MAV_DATA_STREAM_RAW_SENSORS",
    'GPS_RAW_INT' : "MAV_DATA_STREAM_EXTENDED_STATUS",
    'GLOBAL_POSITION_INT' : "MAV_DATA_STREAM_POSITION",
    'ATTITUDE' : "MAV_DATA_STREAM_EXTRA1",
    'SCALED_PRESSURE' : "MAV_DATA_STREAM_RAW_SENSORS",
    'GPS2_RAW' : "MAV_DATA_STREAM_EXTENDED_STATUS",
    'SYS_STATUS' : "MAV_DATA_STREAM_EXTENDED_STATUS"
}
const not_stream_msg = {
    'STATUSTEXT' : 1
}

const param_for_stream = {
    MAV_DATA_STREAM_RAW_SENSORS : 'RAW_SENS',
    MAV_DATA_STREAM_EXTENDED_STATUS : 'EXT_STAT',
    MAV_DATA_STREAM_RC_CHANNELS : 'RC_CHAN',
    MAV_DATA_STREAM_RAW_CONTROLLER : 'RAW_CTRL',
    MAV_DATA_STREAM_POSITION : 'POSITION',
    MAV_DATA_STREAM_EXTRA1 : 'EXTRA1',
    MAV_DATA_STREAM_EXTRA2 : 'EXTRA2',
    MAV_DATA_STREAM_EXTRA3 : 'EXTRA3'
}

const sr_prefix = "SR2_";

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
var mavlink_stream_params = []

var sr_parameters = {}
function fill_sr_parameters(plist) {
    plist.forEach(function bob(entry) {
	sr_parameters[entry.name] = entry.value;
    });
    return true;
}

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
	// determine which streams are required to get the messages we want:
	var mavlink_streams = {}
        for (var msg_name in stream_for_msg) {
	    if (!(msg_name in stream_for_msg)) {
		if (!(msg_name in not_stream_msg)) {
		    console.log("No mapping from " + msg_name + "to stream");
		}
		continue
	    }
	    mavlink_streams[stream_for_msg[msg_name]] = 1;
	}
	// determine which parameters need to be set to get the messages we want:
	var params = {}
	for (var stream in mavlink_streams) {
	    if (param_for_stream[stream] == "undefined") {
		console.log("No mapping from stream " + stream + " to param");
		continue
	    }
	    params[param_for_stream[stream]] = 1;
	}
	mavlink_stream_params = Object.keys(params);
//	console.log("stream_params: " + mavlink_stream_params);

	// start updating our parameter values
	ajax_json_poll(drone_url + "/ajax/command.json?command1=get_param_list(" + sr_prefix + ")", fill_sr_parameters, 1000);
    }
    // check refresh rates:
    var params_to_set = {};
    mavlink_stream_params.forEach(function x(param) {
	var required_rate = 1000/refresh_ms();
	var sr_key = sr_prefix + param;
	if (sr_parameters[sr_key] == "undefined") {
	    return;
	}
	var current_rate = sr_parameters[sr_key];
//	console.log("current=" + current_rate + " required=" + required_rate);
	if (current_rate < required_rate) {
//	    console.log("update required");
	    params_to_set[sr_key] = required_rate;
	}
    });
    mavlink_set_params(params_to_set);

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
    d = (dsec+0.5).toFixed(0);
    var cmd = "set_time_utc(" + d + "," + tz_offset + ")";
    command_send(cmd);
}

// set date every 20s
setInterval(set_sonix_date(), 20000);
