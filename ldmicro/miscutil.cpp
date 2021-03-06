//-----------------------------------------------------------------------------
// Copyright 2007 Jonathan Westhues
//
// This file is part of LDmicro.
//
// LDmicro is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// LDmicro is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with LDmicro.  If not, see <http://www.gnu.org/licenses/>.
//------
//
// Miscellaneous utility functions that don't fit anywhere else. IHEX writing,
// verified memory allocator, other junk.
//-----------------------------------------------------------------------------
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>

#include "ldmicro.h"

// We should display messages to the user differently if we are running
// interactively vs. in batch (command-line) mode.
BOOL RunningInBatchMode = FALSE;

// We are in test mode.
BOOL RunningInTestMode = FALSE;

// Allocate memory on a local heap
HANDLE MainHeap;

// Running checksum as we build up IHEX records.
static int IhexChecksum;

// Try to common a bit of stuff between the dialog boxes, since only one
// can be open at any time.
HWND OkButton;
HWND CancelButton;
BOOL DialogDone;
BOOL DialogCancel;

HFONT MyNiceFont;
HFONT MyFixedFont;

//-----------------------------------------------------------------------------
// printf-like debug function, to the Windows debug log.
//-----------------------------------------------------------------------------
void dbp(char *str, ...)
{
    va_list f;
    char buf[1024*8];
    va_start(f, str);
    vsprintf(buf, str, f);
    OutputDebugString(buf);
//  OutputDebugString("\n");
}

//-----------------------------------------------------------------------------
// Wrapper for AttachConsole that does nothing running under <WinXP, so that
// we still run (except for the console stuff) in earlier versions.
//-----------------------------------------------------------------------------
#define ATTACH_PARENT_PROCESS ((DWORD)-1) // defined in WinCon.h, but only if
                                          // _WIN32_WINNT >= 0x500
BOOL AttachConsoleDynamic(DWORD base)
{
    typedef BOOL WINAPI fptr_acd(DWORD base);
    fptr_acd *fp;

    HMODULE hm = LoadLibrary("kernel32.dll");
    if(!hm) return FALSE;

    fp = (fptr_acd *)GetProcAddress(hm, "AttachConsole");
    if(!fp) return FALSE;

    return fp(base);
}

//-----------------------------------------------------------------------------
void doexit(int status)
{
    exit(status);
}
//-----------------------------------------------------------------------------
// For error messages to the user; printf-like, to a message box.
// For warning messages use ' ' in *str[0], see avr.cpp INT_SET_NPULSE.
//-----------------------------------------------------------------------------
void Error(char *str, ...)
{
    va_list f;
    char buf[1024];
    va_start(f, str);
    vsprintf(buf, str, f);
    dbp(buf);
    if(RunningInBatchMode) {
        AttachConsoleDynamic(ATTACH_PARENT_PROCESS);
        HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
        DWORD written;

        // Indicate that it's an error, plus the output filename
        char str[MAX_PATH+100];
        sprintf(str, "compile error ('%s'): ", CurrentCompileFile);
        WriteFile(h, str, strlen(str), &written, NULL);
        // The error message itself
        WriteFile(h, buf, strlen(buf), &written, NULL);
        // And an extra newline to be safe.
        strcpy(str, "\n");
        WriteFile(h, str, strlen(str), &written, NULL);
    } else {
        HWND h = GetForegroundWindow();
        if(buf[0]==' ')
            MessageBox(h, &buf[1], _("LDmicro Warning"), MB_OK | MB_ICONWARNING);
        else
            MessageBox(h, buf, _("LDmicro Error"), MB_OK | MB_ICONERROR);
    }
}

//-----------------------------------------------------------------------------
// A standard format for showing a message that indicates that a compile
// was successful.
//-----------------------------------------------------------------------------
void CompileSuccessfulMessage(char *str, unsigned int uType)
{
    if(RunningInBatchMode) {
        char str[MAX_PATH+100];
        sprintf(str, "compiled okay, wrote '%s'\n", CurrentCompileFile);

        AttachConsoleDynamic(ATTACH_PARENT_PROCESS);
        HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
        DWORD written;
        WriteFile(h, str, strlen(str), &written, NULL);
    } else if (uType == MB_ICONINFORMATION) {
        MessageBox(MainWindow, str, _("Compile Successful"),
            MB_OK | uType);
    } else {
        MessageBox(MainWindow, str, _("Compile is successful but exceed the memory size !!!"),
            MB_OK | uType);
    }
}

void CompileSuccessfulMessage(char *str)
{
    CompileSuccessfulMessage(str,MB_ICONINFORMATION);
}
//-----------------------------------------------------------------------------
// Check the consistency of the heap on which all the PLC program stuff is
// stored.
//-----------------------------------------------------------------------------
void CheckHeap(char *file, int line)
{
    static unsigned int SkippedCalls;
    static SDWORD LastCallTime;
    SDWORD now = GetTickCount();

    // It slows us down too much to do the check every time we are called;
    // but let's still do the check periodically; let's do it every 70
    // calls or every 20 ms, whichever is sooner.
    if(SkippedCalls < 70 && (now - LastCallTime) < 20) {
        SkippedCalls++;
        return;
    }

    SkippedCalls = 0;
    LastCallTime = now;

    if(!HeapValidate(MainHeap, 0, NULL)) {
        //dbp("file %s line %d", file, line);
        Error("Noticed memory corruption at file '%s' line %d.", file, line);
        oops();
    }
}

//-----------------------------------------------------------------------------
// Like malloc/free, but memsets memory allocated to all zeros. Also TODO some
// checking and something sensible if it fails.
//-----------------------------------------------------------------------------
void *CheckMalloc(size_t n)
{
    ok();
    void *p = HeapAlloc(MainHeap, HEAP_ZERO_MEMORY, n);
    return p;
}
void CheckFree(void *p)
{
    ok();
    HeapFree(MainHeap, 0, p);
}


//-----------------------------------------------------------------------------
// Clear the checksum and write the : that starts an IHEX record.
//-----------------------------------------------------------------------------
void StartIhex(FILE *f)
{
    fprintf(f, ":");
    IhexChecksum = 0;
}

//-----------------------------------------------------------------------------
// Write an octet in hex format to the given stream, and update the checksum
// for the IHEX file.
//-----------------------------------------------------------------------------
void WriteIhex(FILE *f, BYTE b)
{
    fprintf(f, "%02X", b);
    IhexChecksum += b;
}

//-----------------------------------------------------------------------------
// Write the finished checksum to the IHEX file from the running sum
// calculated by WriteIhex.
//-----------------------------------------------------------------------------
void FinishIhex(FILE *f)
{
    IhexChecksum = ~IhexChecksum + 1;
    IhexChecksum = IhexChecksum & 0xff;
    fprintf(f, "%02X\n", IhexChecksum);
}

//-----------------------------------------------------------------------------
// Create a window with a given client area.
//-----------------------------------------------------------------------------
HWND CreateWindowClient(DWORD exStyle, char *className, char *windowName,
    DWORD style, int x, int y, int width, int height, HWND parent,
    HMENU menu, HINSTANCE instance, void *param)
{
    HWND h = CreateWindowEx(exStyle, className, windowName, style, x, y,
        width, height, parent, menu, instance, param);

    RECT r;
    GetClientRect(h, &r);
    width = width - (r.right - width);
    height = height - (r.bottom - height);

    SetWindowPos(h, HWND_TOP, x, y, width, height, 0);

    return h;
}

//-----------------------------------------------------------------------------
// Window proc for the dialog boxes. This Ok/Cancel stuff is common to a lot
// of places, and there are no other callbacks from the children.
//-----------------------------------------------------------------------------
static LRESULT CALLBACK DialogProc(HWND hwnd, UINT msg, WPARAM wParam,
    LPARAM lParam)
{
    switch (msg) {
        case WM_NOTIFY:
            break;

        case WM_COMMAND: {
            HWND h = (HWND)lParam;
            if(h == OkButton && wParam == BN_CLICKED) {
                DialogDone = TRUE;
            } else if(h == CancelButton && wParam == BN_CLICKED) {
                DialogDone = TRUE;
                DialogCancel = TRUE;
            }
            break;
        }

        case WM_CLOSE:
        case WM_DESTROY:
            DialogDone = TRUE;
            DialogCancel = TRUE;
            break;

        default:
            return DefWindowProc(hwnd, msg, wParam, lParam);
    }

    return 1;
}

//-----------------------------------------------------------------------------
// Set the font of a control to a pretty proportional font (typ. Tahoma).
//-----------------------------------------------------------------------------
void NiceFont(HWND h)
{
    SendMessage(h, WM_SETFONT, (WPARAM)MyNiceFont, TRUE);
}

//-----------------------------------------------------------------------------
// Set the font of a control to a pretty fixed-width font (typ. Lucida
// Console).
//-----------------------------------------------------------------------------
void FixedFont(HWND h)
{
    SendMessage(h, WM_SETFONT, (WPARAM)MyFixedFont, TRUE);
}

//-----------------------------------------------------------------------------
// Create our dialog box class, used for most of the popup dialogs.
//-----------------------------------------------------------------------------
void MakeDialogBoxClass(void)
{
    WNDCLASSEX wc;
    memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc);

    wc.style            = CS_BYTEALIGNCLIENT | CS_BYTEALIGNWINDOW | CS_OWNDC |
                          CS_DBLCLKS;
    wc.lpfnWndProc      = (WNDPROC)DialogProc;
    wc.hInstance        = Instance;
    wc.hbrBackground    = (HBRUSH)COLOR_BTNSHADOW;
    wc.lpszClassName    = "LDmicroDialog";
    wc.lpszMenuName     = NULL;
    wc.hCursor          = LoadCursor(NULL, IDC_ARROW);
    wc.hIcon            = (HICON)LoadImage(Instance, MAKEINTRESOURCE(4000),
                            IMAGE_ICON, 32, 32, 0);
    wc.hIconSm          = (HICON)LoadImage(Instance, MAKEINTRESOURCE(4000),
                            IMAGE_ICON, 16, 16, 0);

    RegisterClassEx(&wc);

    MyNiceFont = CreateFont(16, 0, 0, 0, FW_REGULAR, FALSE, FALSE, FALSE,
        ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY,
        FF_DONTCARE, "Tahoma");
    if(!MyNiceFont)
        MyNiceFont = (HFONT)GetStockObject(SYSTEM_FONT);

    MyFixedFont = CreateFont(14, 0, 0, 0, FW_REGULAR, FALSE, FALSE, FALSE,
        ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY,
        FF_DONTCARE, "Lucida Console");
    if(!MyFixedFont)
        MyFixedFont = (HFONT)GetStockObject(SYSTEM_FONT);
}

//-----------------------------------------------------------------------------
// Map an I/O type to a string describing it. Used both in the on-screen
// list and when we write a text file to describe it.
//-----------------------------------------------------------------------------
char *IoTypeToString(int ioType)
{
    switch(ioType) {
        case IO_TYPE_INT_INPUT:         return _("INT input");
        case IO_TYPE_DIG_INPUT:         return _("digital in");
        case IO_TYPE_DIG_OUTPUT:        return _("digital out");
        case IO_TYPE_MODBUS_CONTACT:    return _("modbus contact");
        case IO_TYPE_MODBUS_COIL  :     return _("modbus coil");
        case IO_TYPE_MODBUS_HREG:       return _("modbus Hreg");
        case IO_TYPE_INTERNAL_RELAY:    return _("int. relay");
        case IO_TYPE_UART_TX:           return _("UART tx");
        case IO_TYPE_UART_RX:           return _("UART rx");
        case IO_TYPE_PWM_OUTPUT:        return _("PWM out");
        case IO_TYPE_TCY:               return _("cyclic on/off");
        case IO_TYPE_TON:               return _("turn-on delay");
        case IO_TYPE_TOF:               return _("turn-off delay");
        case IO_TYPE_RTO:               return _("retentive timer");
        case IO_TYPE_COUNTER:           return _("counter");
        case IO_TYPE_GENERAL:           return _("general var");
        case IO_TYPE_PERSIST:           return _("saved var");
        case IO_TYPE_BCD:               return _("BCD var");
        case IO_TYPE_STRING:            return _("string var");
        case IO_TYPE_TABLE:             return _("table in flash");
        case IO_TYPE_READ_ADC:          return _("adc input");
        case IO_TYPE_PORT_INPUT:        return _("PORT input");
        case IO_TYPE_PORT_OUTPUT:       return _("PORT output");
        default:                        return _("<corrupt!>");
    }
}

//-----------------------------------------------------------------------------
// Get a pin number, portName, pinName for a given I/O; for digital ins and outs and analog ins,
// this is easy, but for PWM and UART and interrupts this is forced (by what peripherals
// are available) so we look at the characteristics of the MCU that is in
// use.
//-----------------------------------------------------------------------------
void PinNumberForIo(char *dest, PlcProgramSingleIo *io, char *portName, char *pinName)
{
    if(!dest) return;

    strcpy(dest, "");
    if(portName)
        strcpy(portName, "");
    if(pinName)
        strcpy(pinName, "");

    if(!io) return;

    int type = io->type;
    int pin = io->pin;
    McuIoPinInfo *iop;
    if(type == IO_TYPE_DIG_INPUT || type == IO_TYPE_DIG_OUTPUT
    || type == IO_TYPE_READ_ADC)
    {
        if(pin == NO_PIN_ASSIGNED) {
            strcpy(dest, _("(not assigned)"));
            if(portName)
                strcpy(portName, _("(not assigned)"));
            if(pinName)
                strcpy(pinName, _("(not assigned)"));
        } else {
            sprintf(dest, "%d", pin);
            if(portName) {
                if(UartFunctionUsed() && Prog.mcu) {
                    if((Prog.mcu->uartNeeds.rxPin == pin) ||
                       (Prog.mcu->uartNeeds.txPin == pin))
                    {
                        strcpy(portName, _("<UART needs!>"));
                        return;
                    }
                }
                if(PwmFunctionUsed() && Prog.mcu) {
                    if(Prog.mcu->pwmNeedsPin == pin) {
                        strcpy(portName, _("<PWM needs!>"));
                        return;
                    }
                }
                /*
                if(QuadEncodFunctionUsed() && Prog.mcu) {
                    if((Prog.mcu->IntNeeds.int0 == pin)
                    || (Prog.mcu->IntNeeds.int1 == pin) )
                    {
                        strcpy(portName, _("<INT needs!>"));
                        return;
                    }
                }
                */
                iop = PinInfo(pin);
                if(iop && Prog.mcu)
                    sprintf(portName, "%c%c%d",
                        Prog.mcu->portPrefix, iop->port, iop->bit);
                else
                    strcpy(portName, _("<not an I/O!>"));
            }
            if(pinName) {
                if(UartFunctionUsed() && Prog.mcu) {
                    if((Prog.mcu->uartNeeds.rxPin == pin) ||
                       (Prog.mcu->uartNeeds.txPin == pin))
                    {
                        strcpy(pinName, _("<UART needs!>"));
                        return;
                    }
                }
                if(PwmFunctionUsed() && Prog.mcu) {
                    if(Prog.mcu->pwmNeedsPin == pin) {
                        strcpy(pinName, _("<PWM needs!>"));
                        return;
                    }
                }
                /*
                if(QuadEncodFunctionUsed() && Prog.mcu) {
                    if((Prog.mcu->IntNeeds.int0 == pin)
                    || (Prog.mcu->IntNeeds.int1 == pin) )
                    {
                        strcpy(pinName, _("<INT needs!>"));
                        return;
                    }
                }
                */
                iop = PinInfo(pin);
                if(iop && Prog.mcu) {
                    if((iop->pinName) && strlen(iop->pinName))
                      sprintf(pinName, "%s", iop->pinName);
                } else
                    strcpy(pinName, _("<not an I/O!>"));
            }
        }
    } else if(type == IO_TYPE_INT_INPUT && Prog.mcu) {
        if(Prog.mcu->interruptCount == 0) {
            strcpy(dest, _("<no INTs!>"));
            if(portName)
                strcpy(portName, _("<no INTs!>"));
            if(pinName)
                strcpy(pinName, _("<no INTs!>"));
        } else {
            sprintf(dest, "%d", pin);
            iop = PinInfo(pin);
            if(iop) {
                if(portName) {
                    sprintf(portName, "%c%c%d",
                        Prog.mcu->portPrefix, iop->port, iop->bit);
                if(iop->pinName)
                    sprintf(pinName, "%s", iop->pinName);
            } else
                if(portName)
                    strcpy(portName, _("<not an I/O!>"));
                if(pinName)
                    strcpy(pinName, _("<not an I/O!>"));
            }
        }
    } else if(type == IO_TYPE_UART_TX && Prog.mcu) {
        if(Prog.mcu->uartNeeds.txPin == 0) {
            strcpy(dest, _("<no UART!>"));
            if(portName)
                strcpy(portName, _("<no UART!>"));
            if(pinName)
                strcpy(pinName, _("<no UART!>"));
        } else {
            sprintf(dest, "%d", Prog.mcu->uartNeeds.txPin);
            iop = PinInfo(Prog.mcu->uartNeeds.txPin);
            if(iop) {
                if(portName)
                    sprintf(portName, "%c%c%d",
                      Prog.mcu->portPrefix, iop->port, iop->bit);
                 if(pinName)
                    if(iop->pinName)
                        sprintf(pinName, "%s", iop->pinName);
            } else {
                if(portName)
                    strcpy(portName, _("<not an I/O!>"));
                if(pinName)
                    strcpy(pinName, _("<not an I/O!>"));
            }
        }
    } else if(type == IO_TYPE_UART_RX && Prog.mcu) {
        if(Prog.mcu->uartNeeds.rxPin == 0) {
            strcpy(dest, _("<no UART!>"));
            if(portName)
                strcpy(portName, _("<no UART!>"));
            if(pinName)
                strcpy(pinName, _("<no UART!>"));
        } else {
            sprintf(dest, "%d", Prog.mcu->uartNeeds.rxPin);
            iop = PinInfo(Prog.mcu->uartNeeds.rxPin);
            if(iop) {
                if(portName)
                    sprintf(portName, "%c%c%d",
                      Prog.mcu->portPrefix, iop->port, iop->bit);
                 if(pinName)
                    if(iop->pinName)
                        sprintf(pinName, "%s", iop->pinName);
            } else {
                if(portName)
                    strcpy(portName, _("<not an I/O!>"));
                if(pinName)
                    strcpy(pinName, _("<not an I/O!>"));
            }
        }
    } else if(type == IO_TYPE_PWM_OUTPUT && Prog.mcu) {
#if 1
        if(!McuPWM()) {
            strcpy(dest, _("<no PWM!>"));
            if(portName)
                strcpy(portName, _("<no PWM!>"));
            if(pinName)
                strcpy(pinName, _("<no PWM!>"));
        } else {

            sprintf(dest, "%d", pin);
            iop = PinInfo(pin);
            if(iop) {
                if(portName)
                    sprintf(portName, "%c%c%d",
                      Prog.mcu->portPrefix, iop->port, iop->bit);
                if(pinName)
                    if(iop->pinName)
                        sprintf(pinName, "%s", iop->pinName);
            } else {
                if(portName)
                    strcpy(portName, _("<not an I/O!>"));
                if(pinName)
                    strcpy(pinName, _("<not an I/O!>"));
            }
        }
#else
        int pin = io->pin;
        if (pin == NO_PIN_ASSIGNED) {
            strcpy(dest, _("(not assigned)"));
        }
        else {
            sprintf(dest, "%d", pin);
        }
#endif
    //} else if((type == IO_TYPE_STRING)) {
    }
}
//-----------------------------------------------------------------------------
char *GetPinName(int pin, char *pinName)
{
    sprintf(pinName, "");
    int i;
    if(Prog.mcu)
    if(pin != NO_PIN_ASSIGNED)
    for(i = 0; i < Prog.mcu->pinCount; i++)
        if(Prog.mcu->pinInfo[i].pin==pin)
            if(Prog.mcu && (Prog.mcu->portPrefix == 'L') && (Prog.io.assignment[i].pin))
                sprintf(pinName, "%s",
                    PinToName(Prog.io.assignment[i].pin));
            else
                if((Prog.mcu->pinInfo[i].pinName) && strlen(Prog.mcu->pinInfo[i].pinName))
                  sprintf(pinName, "%s", Prog.mcu->pinInfo[i].pinName);
                else
                  sprintf(pinName, "%c%c%d",
                    Prog.mcu->portPrefix,
                    Prog.mcu->pinInfo[i].port,
                    Prog.mcu->pinInfo[i].bit);
    return pinName;
}

//-----------------------------------------------------------------------------
void PinNumberForIo(char *dest, PlcProgramSingleIo *io)
{
    PinNumberForIo(dest, io, NULL, NULL);
}

//-----------------------------------------------------------------------------
int NameToPin(char *pinName)
{
    int i;
    if(Prog.mcu)
       for(i = 0; i < Prog.mcu->pinCount; i++)
           if(strcmp(Prog.mcu->pinInfo[i].pinName,pinName)==0)
               return Prog.mcu->pinInfo[i].pin;
    return 0;
}
//-----------------------------------------------------------------------------
char *PinToName(int pin)
{
    int i;
    if(Prog.mcu)
        for(i = 0; i < Prog.mcu->pinCount; i++)
            if(Prog.mcu->pinInfo[i].pin==pin)
                return Prog.mcu->pinInfo[i].pinName;
    return "";
}
//-----------------------------------------------------------------------------
McuIoPinInfo *PinInfo(int pin)
{
    int i;
    if(Prog.mcu)
        for(i = 0; i < Prog.mcu->pinCount; i++)
            if(Prog.mcu->pinInfo[i].pin==pin)
                return &(Prog.mcu->pinInfo[i]);
    return NULL;
}

//-----------------------------------------------------------------------------
McuIoPinInfo *PinInfoForName(char *name)
{
    int i;
    if(Prog.mcu)
        for(i = 0; i < Prog.io.count; i++)
            if(strcmp(Prog.io.assignment[i].name, name)==0)
                return PinInfo(Prog.io.assignment[i].pin);
    return NULL;
}

//-----------------------------------------------------------------------------
McuPwmPinInfo *PwmPinInfo(int pin)
{
    int i;
    if(Prog.mcu)
        for(i = 0; i < Prog.mcu->pwmCount; i++)
            if(Prog.mcu->pwmInfo[i].pin==pin)
                return &(Prog.mcu->pwmInfo[i]);
    return NULL;
}

//-----------------------------------------------------------------------------
McuPwmPinInfo *PwmPinInfoForName(char *name)
{
    int i;
    if(Prog.mcu)
        for(i = 0; i < Prog.io.count; i++) {
            if(strcmp(Prog.io.assignment[i].name, name)==0)
                return PwmPinInfo(Prog.io.assignment[i].pin);
        }
    return NULL;
}

//-----------------------------------------------------------------------------
McuAdcPinInfo *AdcPinInfo(int pin)
{
    int i;
    if(Prog.mcu)
        for(i = 0; i < Prog.mcu->adcCount; i++)
            if(Prog.mcu->adcInfo[i].pin==pin)
                return &(Prog.mcu->adcInfo[i]);
    return NULL;
}

//-----------------------------------------------------------------------------
McuAdcPinInfo *AdcPinInfoForName(char *name)
{
    int i;
    if(Prog.mcu)
        for(i = 0; i < Prog.io.count; i++)
            if(strcmp(Prog.io.assignment[i].name, name)==0)
                return AdcPinInfo(Prog.io.assignment[i].pin);
    return NULL;
}

//-----------------------------------------------------------------------------
BOOL IsInterruptPin(int pin)
{
    int i;
    if(Prog.mcu)
        for(i = 0; i < Prog.mcu->interruptCount; i++)
            if(Prog.mcu->interruptInfo[i].pin==pin)
                return TRUE;
    return FALSE;
}

//-----------------------------------------------------------------------------
int ishobdigit(int c)
{
    if((isxdigit(c)) || (toupper(c)=='X') || (toupper(c)=='O')/* || (toupper(c)=='B')*/)
        return 1;
    return 0;
}
//-----------------------------------------------------------------------------
int isal_num(int c)
{
    return isalnum(c) || c == '_';
}
//-----------------------------------------------------------------------------
int isalpha_(int c)
{
    return isalpha(c) || c == '_';
}
//-----------------------------------------------------------------------------
int isname(char *name)
{
    if(!isalpha_(*name))
        return 0;
    char *s = name;
    while(*s) {
        if(!isal_num(*s))
            return 0;
        s++;
    }
    return 1;
}

//-----------------------------------------------------------------------------
size_t strlenalnum(const char *str)
{
    size_t r=0;
    if(str) r=strlen(str);
    if(r) {
        while(*str) {
            if(isdigit(*str) || isalpha(*str))
                break;
            str++;
        }
        r=strlen(str);
        while(r) {
            if(isdigit(str[r-1]) || isalpha(str[r-1]))
                break;
            r--;
        }
    }
    return r;
}

//-----------------------------------------------------------------------------
void CopyBit(DWORD *Dest, int bitDest, DWORD Src, int bitSrc)
{
    if(Src & (1<<bitSrc))
        *Dest |= 1 << bitDest;
    else
        *Dest &= ~(1 << bitDest);
}

//-----------------------------------------------------------------------------
char *strDelSpace(char *dest, char *src)
{
    char *s;
    if(src)
        s = src;
    else
        s = dest;
    int i = 0;
    for(; *s; s++)
        if(!isspace(*s))
            dest[i++] = *s;
    dest[i] = '\0';
    return dest;
}

char *strDelSpace(char *dest)
{
    return strDelSpace(dest, NULL);
}

