// Patch 5: Title screen "press any button" — check all device slots
// The title screen only checks gamepad_button_check_any(0), missing the virtual
// controller if it landed on a different slot. Check slots 0-3 to be safe.

EnsureDataLoaded();
GlobalDecompileContext ctx = new(Data);
var dSettings = new Underanalyzer.Decompiler.DecompileSettings();
var importGroup = new UndertaleModLib.Compiler.CodeImportGroup(Data, ctx, dSettings);

importGroup.QueueFindReplace("gml_Object_obj_titleimage_Draw_0",
    "gamepad_button_check_any(0)",
    "gamepad_button_check_any(0) || gamepad_button_check_any(1) || gamepad_button_check_any(2) || gamepad_button_check_any(3)");

importGroup.Import();
ScriptMessage("Patch 5 done: title screen button check fixed");
