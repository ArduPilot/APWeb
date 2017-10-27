var charts = {};
var chart_lines = {};

function chart_range(range) {
    var round_val = Math.round(((range.max+5) - (range.min-5))/7);
    var max = Math.round(range.max+round_val);
    var min = Math.round(range.min-round_val);
    return {min: min, max: max};
}

// create a new chart with a list of MAVLink variables as lines
function create_chart(canvass_name, variables) {
    var colors = [ "red", "green", "blue", "orange" ];
    var settings = { grid : { fillStyle: "DarkGrey" },
                     yRangeFunction : chart_range };
    charts[canvass_name] = new SmoothieChart(settings);
    for (var i=0; i<variables.length; i++) {
        if (chart_lines[variables[i]] == null) {
            chart_lines[variables[i]] = [];
        }
        var ts = new TimeSeries();
        chart_lines[variables[i]].push(ts);
        charts[canvass_name].addTimeSeries(ts, { strokeStyle : colors[i] });
    }
    charts[canvass_name].streamTo(document.getElementById(canvass_name), 500);
}

