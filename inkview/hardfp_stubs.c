/* Link-only InkView stub for hard-float firmware. The real symbols are supplied
 * by /ebrmain/lib/libinkview.so on the reader. */
#define STUB(name) void name(void) {}

STUB(ClearScreen)
STUB(CloseApp)
STUB(CloseFont)
STUB(DimArea)
STUB(Dialog)
STUB(Dialog3)
STUB(DrawApplicationCaption)
STUB(DrawBitmapRect)
STUB(DrawLine)
STUB(DrawPanel)
STUB(DrawRect)
STUB(DrawSymbol)
STUB(DrawTextRect)
STUB(FillArea)
STUB(FullUpdate)
STUB(GetCaptionHeight)
STUB(InitPanel)
STUB(InkViewMain)
STUB(LoadBitmap)
STUB(LoadJPEG)
STUB(LoadPNGStretch)
STUB(LoadPNG8)
STUB(Message)
STUB(OpenFont)
STUB(PanelHeight)
STUB(ScreenHeight)
STUB(ScreenWidth)
STUB(SetClip)
STUB(SetWeakTimer)
STUB(SetApplicationCaptionHeight)
STUB(SetFont)
STUB(SetPanelType)
STUB(ShowHourglass)
STUB(StringWidth)
STUB(TextRectHeight)
STUB(HideHourglass)
STUB(currentLang)
STUB(iv_get_default_font)
STUB(iv_ipc_request)

void InitInkview(int flags) { (void)flags; }
void GetActiveTask(int *task, int *subtask)
{
    if (task) *task = 0;
    if (subtask) *subtask = 0;
}
void *GetTaskInfo(int task) { (void)task; return 0; }
int get_keylock(void) { return 0; }
