// Patch 6 (optional): Debug overlay
// Adds a DrawGUI event to obj_time that shows gamepad state on screen.
// Useful for troubleshooting input issues. Don't apply this for a release build.
//
// Shows: j_ch, devcount, gamepad_is_connected(0/1), axis value, button state

EnsureDataLoaded();
GlobalDecompileContext ctx = new(Data);
var dSettings = new Underanalyzer.Decompiler.DecompileSettings();
var importGroup = new UndertaleModLib.Compiler.CodeImportGroup(Data, ctx, dSettings);

var obj_time = Data.GameObjects.ByName("obj_time");
importGroup.QueueAppend(
    obj_time.EventHandlerFor(EventType.Draw, EventSubtypeDraw.DrawGUI, Data),
@"draw_set_font(-1);
draw_set_halign(fa_left);
draw_set_color(c_yellow);
draw_text(10, 10, ""j_ch="" + string(j_ch));
draw_text(10, 30, ""devcount="" + string(gamepad_get_device_count()));
draw_text(10, 50, ""con0="" + string(gamepad_is_connected(0)));
draw_text(10, 70, ""con1="" + string(gamepad_is_connected(1)));
draw_text(10, 90, ""axisH="" + string(gamepad_axis_value(j_ch - 1, gp_axislh)));
draw_text(10, 110, ""btn0_d0="" + string(gamepad_button_check(0, global.button0)));
draw_text(10, 130, ""btn0_d1="" + string(gamepad_button_check(1, global.button0)));
");

importGroup.Import();
ScriptMessage("Patch 6 done: debug overlay added");
