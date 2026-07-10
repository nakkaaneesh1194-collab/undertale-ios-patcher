// Patch 2: Fix j_ch rescan in obj_time Step event
// The original fallback uses gamepad_get_device_count() which returns 5 on iOS
// (system virtual devices), setting j_ch=5 and breaking input.
// Instead: scan up to 8 slots for any button press, fall back to slot 0 (j_ch=1).

EnsureDataLoaded();
GlobalDecompileContext ctx = new(Data);
var dSettings = new Underanalyzer.Decompiler.DecompileSettings();
var importGroup = new UndertaleModLib.Compiler.CodeImportGroup(Data, ctx, dSettings);

importGroup.QueueFindReplace("gml_Object_obj_time_Step_1",
@"if (global.osflavor >= 4 && jt == 0)
{
    var _best = -1;
    for (var _i = 0; _i < gamepad_get_device_count(); _i++)
    {
        if (gamepad_button_check_any(_i))
        {
            _best = _i;
            break;
        }
    }
    if (_best >= 0)
    {
        j_ch = _best + 1;
    }
    else if (gamepad_get_device_count() > 0)
    {
        j_ch = gamepad_get_device_count();
    }
}",
@"if (global.osflavor >= 4 && jt == 0)
{
    var _best = -1;
    for (var _i = 0; _i < 8; _i++)
    {
        if (gamepad_button_check_any(_i))
        {
            _best = _i;
            break;
        }
    }
    if (_best >= 0)
    {
        j_ch = _best + 1;
    }
    else
    {
        j_ch = 1;
    }
}");

importGroup.Import();
ScriptMessage("Patch 2 done: j_ch rescan fixed");
