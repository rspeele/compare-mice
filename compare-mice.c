#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0601
#include<windows.h>
#include<stdio.h>
#include<stdlib.h>
#include<math.h>
#include<time.h>

#define USAGE_PAGE_GENERIC_DESKTOP 0x01
#define USAGE_MOUSE 0x02
#define USAGE_KEYBOARD 0x06

typedef unsigned int uint;
typedef struct winrawdev_s
{
    HANDLE device;
    DWORD type;
    struct { int dx, dy; } history;
    unsigned char name[128];
    int reports;
    clock_t lastreport;
    clock_t minreportdelta;
} winrawdev;

// Open registry key for reading corresponding to
// the device with RIDI_DEVICENAME of /name/.
//
// Return 0 on success, -1 on failure.
int opendevicekey(char *name, HKEY *handle)
{
    static char regpath[512];
    const char *base = "SYSTEM\\CurrentControlSet\\Enum\\";
    while(*name && (*name == '\\' || *name == '?')) name++;
    int i, k, p;
    for(i = 0; base[i] && i < sizeof(regpath); i++)
    {
        regpath[i] = base[i];
    }
    for(k = 0, p = 0; name[k] && i < sizeof(regpath); (void)(++i && ++k))
    {
        char c = name[k];
        if(c == '#')
        {
            if(++p > 2) break;
            regpath[i] = '\\';
        }
        else regpath[i] = c;
    }
    regpath[i] = '\0';
    LONG open
        = RegOpenKeyEx(HKEY_LOCAL_MACHINE,
                       regpath,
                       0,
                       KEY_READ,
                       handle);
    return open == ERROR_SUCCESS ? 0 : -1;
}

void trimleft(unsigned char until, unsigned char *modify)
{
    unsigned char *current = modify;
    while (*current && *current++ != until);
    while ((*modify++ = *current++));
}

// Fill in extra info for /dev/, assuming /dev/.device is set.
void fillinfo(winrawdev *dev)
{
    *dev->name = '\0'; // clear name
    static char dname[256];
    UINT len = sizeof(dname);
    UINT info
        = GetRawInputDeviceInfo(dev->device,
                                RIDI_DEVICENAME,
                                &dname,
                                &len);
    if(info == (UINT)-1)
    {
        fprintf(stderr, "Failed to get device name for %u\n", dev->device);
        return;
    }
    HKEY reg;
    if(opendevicekey(dname, &reg) < 0)
    {
        fprintf(stderr, "Failed to open registry key for device %u (%s)\n", dev->device, dname);
        return;
    }
    DWORD type;
    ULONG size = sizeof(dev->name);
    RegQueryValueExA(reg,
                     "DeviceDesc",
                     NULL,
                     &type,
                     dev->name,
                     &size);
    RegCloseKey(reg);
    if(size > 0) dev->name[size-1] = '\0';
    trimleft(';', dev->name);
}

// Populate /devs/ with info for raw input devices connected to the
// system, not to exceed /length/ devices.
//
// Return the number of devices populated.
int listdevices(winrawdev *devs, int length)
{
    PRAWINPUTDEVICELIST rids;
    UINT numrids;
    UINT list = GetRawInputDeviceList(NULL, &numrids, sizeof(RAWINPUTDEVICELIST));
    if (list == (UINT)-1) return -1;
    rids = (PRAWINPUTDEVICELIST)malloc(numrids * sizeof(RAWINPUTDEVICELIST));
    if (!rids) return -2;
    list = GetRawInputDeviceList(rids, &numrids, sizeof(RAWINPUTDEVICELIST));
    if (list == (UINT)-1)
    {
        free(rids);
        return -3;
    }
    int fill = min(numrids, length);
    int i;
    for (i = 0; i < fill; i++)
    {
        if(i >= length) break;
        devs[i].device = rids[i].hDevice;
        devs[i].type = rids[i].dwType;
        fillinfo(devs+i);
    }
    free(rids);
    return fill;
}

void showdevices(winrawdev *devs, int length)
{
    printf("--------------------\n");
    printf("Devices:\n");
    int i;
    for (i = 0; i < length; i++)
    {
        printf("\t%s(%u)\n", devs[i].name, devs[i].device);
    }
    printf("--------------------\n");
}

// Register to receive input from desktop devices matching /usage/.
//
// Return 0 on success.
int registerdevice(RAWINPUTDEVICE *rid, DWORD usage, HWND window)
{
    rid->usUsagePage = USAGE_PAGE_GENERIC_DESKTOP;
    rid->usUsage = usage;
    rid->dwFlags = RIDEV_NOLEGACY|RIDEV_INPUTSINK;
    rid->hwndTarget = window;
    return (RegisterRawInputDevices(rid, 1, sizeof(*rid)) == FALSE);
}

void handlekeyboard(winrawdev *device, int vkey, unsigned int flags);
void handlemouse(winrawdev *device, int dx, int dy);
winrawdev *lookupdev(HANDLE device);
int handlerawinput(LPARAM lparam)
{
    RAWINPUT raw;
    UINT size = sizeof(raw);
    // read into buffer
    UINT read =
        GetRawInputData((HRAWINPUT)lparam,
                        RID_INPUT,
                        &raw,
                        &size,
                        sizeof(RAWINPUTHEADER));
    if (read == (UINT)-1)
    {
        fprintf(stderr, "error getting raw input data");
        return -1;
    }
    if (read > size || read < sizeof(RAWINPUTHEADER))
    {
        fprintf(stderr, "size mismatch %d (expected %d)\n", read, size);
        return -1;
    }
    winrawdev *dev = lookupdev(raw.header.hDevice);
    if (!dev)
    {
        fprintf(stderr, "Got event from unrecognized device.\nTry restarting program to detect new devices.\n");
    }
    switch (raw.header.dwType)
    {
    case RIM_TYPEMOUSE:
        handlemouse(dev, raw.data.mouse.lLastX, raw.data.mouse.lLastY);
        break;
    case RIM_TYPEKEYBOARD: // note, keyboard RI_KEY_MAKE events repeat when key is held down
        handlekeyboard(dev, raw.data.keyboard.VKey, raw.data.keyboard.Flags);
        break;
    default:
        return -1;
    }
    return 0;
}

LRESULT CALLBACK wndproc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
    switch (msg)
    {
    case WM_INPUT:
        if(!handlerawinput(lparam)) break;
    default:
        return DefWindowProc(hwnd, msg, wparam, lparam);
    }
    return 0; // return 0 to indicate message was processed
}

const char *windowtitle = "rawinput";
#define maxdevices 16
static winrawdev devices[maxdevices];
static int numdevices = 0;
static int paused = 0;

winrawdev *lookupdev(HANDLE device)
{
    int i;
    for(i = 0; i < numdevices; i++)
    {
        if (devices[i].device == device) return &devices[i];
    }
    return NULL;
}
inline const float magnitude(const int dx, const int dy)
{
    return sqrtf((float)(dx * dx + dy * dy));
}
// Return the factor the sensitivity with /from/ should be multiplied by
// to get the same sensitivity with /to/.
const float conversion(const winrawdev *const from, const winrawdev *const to)
{
    const float tmag = magnitude(to->history.dx, to->history.dy);
    const float fmag = magnitude(from->history.dx, from->history.dy);
    return tmag ? fmag / tmag : tmag;
}
const float anglediff(const winrawdev *const from, const winrawdev *const to)
{
    const float ato = atan2f(to->history.dx, to->history.dy);
    const float afrom = atan2f(from->history.dx, from->history.dy);
    return ato - afrom;
}
const char *describeanglediff(float adiff)
{
    const float a = fabs(adiff);
    return
        a == 0 ? "Perfect"
        : a < 0.01 ? "Excellent"
        : a < 0.05 ? "Very Good"
        : a < 0.10 ? "Good"
        : a < 0.15 ? "Adequate"
        : a < 0.20 ? "Questionable"
        : a < 0.30 ? "Bad"
        : a < 0.50 ? "Terrible"
        : "You didn't move them together, did you?"
        ;
}
void showconversions(const winrawdev *const from)
{
    if (from->type != RIM_TYPEMOUSE || !(from->history.dx || from->history.dy)) return;
    printf("Device %s(%u)\n", from->name, from->device);
    double hz = 1.0 / ((double)(from->minreportdelta) / CLOCKS_PER_SEC);
    printf
        ("\tHz (max): %.0f\n"
         "\tDelta (x, y): (%d, %d)\n"
         , hz
         , from->history.dx
         , from->history.dy
        );
    if (numdevices < 2) return;
    int i;
    printf("  To go from %s(%u)\n", from->name, from->device);
    for (i = 0; i < numdevices; i++)
    {
        if (devices[i].type == RIM_TYPEMOUSE &&
           &devices[i] != from &&
           (devices[i].history.dx || devices[i].history.dy))
        {
            float adiff = anglediff(from, &devices[i]);
            printf
                ("\tto %s(%u):\n"
                 "\t\tMultiply sensitivity by: %.3f\n"
                 "\t\tAngle offset: %.2f (%s)\n\n"
                 , devices[i].name
                 , devices[i].device
                 , conversion(from, &devices[i])
                 , adiff
                 , describeanglediff(adiff)
                );
        }
    }
}
void handlekeyboard(winrawdev *device, int vkey, unsigned int flags)
{
    if (flags & RI_KEY_BREAK) return;
    switch(vkey)
    {
    case VK_DELETE:
    case 'R':
    {
        int i;
        for (i = 0; i < numdevices; i++)
        {
            devices[i].history.dx = devices[i].history.dy = 0;
        }
        printf("History reset.\n");
        break;
    }
    case VK_SPACE:
    {
        paused ^= 1;
        printf("%saused.\n", paused ? "P" : "Unp");
        break;
    }
    case 'S':
    {
        int i;
        for (i = 0; i < numdevices; i++)
        {
            showconversions(&devices[i]);
        }
    }
    }
}

void handlemouse(winrawdev *device, int dx, int dy)
{
    if (paused) return;
    device->history.dx += dx;
    device->history.dy += dy;
    if (!device->reports)
    {
        printf("Got mouse movement from %s(%u)\n", device->name, device->device);
        device->minreportdelta = -1;
    }
    else
    {
        clock_t delta = clock() - device->lastreport;
        if (delta != 0
            && (device->minreportdelta == -1 || delta < device->minreportdelta))
        {
            device->minreportdelta = delta;
        }
    }
    device->reports++;
    device->lastreport = clock();
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hignore, LPSTR lpignore, int nCmdShow)
{
    printf(
        "Directions:\n"
        "\tMove both mice together as one, then hit S to show stats.\n"
        "Controls:\n"
        "\tS = Show stats.\n"
        "\tR = Reset history.\n"
        "\t    Useful if you messed up and want to run another test.\n"
        "\tSPACE = Pause/unpause recording mouse movements.\n"
        );
    numdevices = listdevices(devices, maxdevices);
    showdevices(devices, numdevices);

    WNDCLASS wc;
    HWND hwnd;
    MSG msg;

    wc.style = CS_HREDRAW|CS_VREDRAW;
    wc.lpfnWndProc = wndproc;
    wc.cbClsExtra = 0;
    wc.cbWndExtra = 0;
    wc.hInstance = hInst;
    wc.hIcon = LoadIcon(NULL, IDI_WINLOGO);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)COLOR_WINDOWFRAME;
    wc.lpszMenuName = NULL;
    wc.lpszClassName = windowtitle;

    if (!RegisterClass(&wc))
        return 0;

    hwnd =
        CreateWindow(windowtitle, // class name
                     windowtitle, // window name
                     0,
                     CW_USEDEFAULT, // x
                     CW_USEDEFAULT, // y
                     0, // width
                     0, // height
                     NULL, // parent
                     NULL, // menu
                     hInst, // instance
                     NULL); // lpParam

    if (!hwnd)
        return -1;

    RAWINPUTDEVICE mouse;
    registerdevice(&mouse, USAGE_MOUSE, hwnd);
    RAWINPUTDEVICE keyboard;
    registerdevice(&keyboard, USAGE_KEYBOARD, hwnd);

    UpdateWindow(hwnd);

    while (GetMessage(&msg, NULL, 0, 0) > 0)
    {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    return 0;
}
