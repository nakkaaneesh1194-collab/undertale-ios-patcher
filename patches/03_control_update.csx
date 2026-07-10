// Patch 3: Fix control_update hardcoded device 0
// control_update checks gamepad_button_check(0, ...) for all buttons.
// Device 0 is hardcoded but j_ch tracks the active device (1-indexed).
// Fix: use obj_time.j_ch - 1 as the device index.

EnsureDataLoaded();
GlobalDecompileContext ctx = new(Data);
var dSettings = new Underanalyzer.Decompiler.DecompileSettings();
var importGroup = new UndertaleModLib.Compiler.CodeImportGroup(Data, ctx, dSettings);

importGroup.QueueFindReplace("gml_Script_control_update",
    "gamepad_button_check(0, global.button0)",
    "gamepad_button_check(obj_time.j_ch - 1, global.button0)");

importGroup.QueueFindReplace("gml_Script_control_update",
    "gamepad_button_check(0, global.button1)",
    "gamepad_button_check(obj_time.j_ch - 1, global.button1)");

importGroup.QueueFindReplace("gml_Script_control_update",
    "gamepad_button_check(0, global.button2)",
    "gamepad_button_check(obj_time.j_ch - 1, global.button2)");

importGroup.Import();
ScriptMessage("Patch 3 done: control_update device index fixed");
