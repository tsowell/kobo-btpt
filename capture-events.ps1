<#
.SYNOPSIS
    Captura eventos de comandos Bluetooth (Teclado, Multimédia e Rato/Toque) no Windows
    e mostra os códigos equivalentes do Linux (<linux/input-event-codes.h>) para o Kobo (.btpt).

.EXAMPLE
    .\capture-events.ps1
#>

Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing

$csharp = @"
using System;
using System.Runtime.InteropServices;
using System.Windows.Forms;

public class RawInputCaptureWindow : Form
{
    public event Action<string> OnEventCaptured;

    [StructLayout(LayoutKind.Sequential)]
    struct RAWINPUTDEVICE
    {
        public ushort usUsagePage;
        public ushort usUsage;
        public uint dwFlags;
        public IntPtr hwndTarget;
    }

    [StructLayout(LayoutKind.Sequential)]
    struct RAWINPUTHEADER
    {
        public uint dwType;
        public uint dwSize;
        public IntPtr hDevice;
        public IntPtr wParam;
    }

    [StructLayout(LayoutKind.Explicit)]
    struct RAWMOUSE
    {
        [FieldOffset(0)] public ushort usFlags;
        [FieldOffset(4)] public uint ulButtons;
        [FieldOffset(4)] public ushort usButtonFlags;
        [FieldOffset(6)] public ushort usButtonData;
        [FieldOffset(8)] public uint ulRawButtons;
        [FieldOffset(12)] public int lLastX;
        [FieldOffset(16)] public int lLastY;
        [FieldOffset(20)] public uint ulExtraInformation;
    }

    [StructLayout(LayoutKind.Sequential)]
    struct RAWKEYBOARD
    {
        public ushort MakeCode;
        public ushort Flags;
        public ushort Reserved;
        public ushort VKey;
        public uint Message;
        public uint ExtraInformation;
    }

    [DllImport("User32.dll", SetLastError = true)]
    static extern bool RegisterRawInputDevices(RAWINPUTDEVICE[] pRawInputDevices, uint uiNumDevices, uint cbSize);

    [DllImport("User32.dll", SetLastError = true)]
    static extern uint GetRawInputData(IntPtr hRawInput, uint uiCommand, IntPtr pData, ref uint pcbSize, uint cbSizeHeader);

    const int WM_INPUT = 0x00FF;
    const uint RID_INPUT = 0x10000003;
    const uint RIDEV_INPUTSink = 0x00000100;

    private int accumX = 0;
    private int accumY = 0;
    private bool buttonDown = false;

    protected override void OnHandleCreated(EventArgs e)
    {
        base.OnHandleCreated(e);
        RAWINPUTDEVICE[] devs = new RAWINPUTDEVICE[3];
        // Mouse (Generic Desktop 0x01, Mouse 0x02)
        devs[0].usUsagePage = 0x01; devs[0].usUsage = 0x02; devs[0].dwFlags = RIDEV_INPUTSink; devs[0].hwndTarget = this.Handle;
        // Keyboard (Generic Desktop 0x01, Keyboard 0x06)
        devs[1].usUsagePage = 0x01; devs[1].usUsage = 0x06; devs[1].dwFlags = RIDEV_INPUTSink; devs[1].hwndTarget = this.Handle;
        // Consumer Control / Media Keys (0x0C, 0x01)
        devs[2].usUsagePage = 0x0C; devs[2].usUsage = 0x01; devs[2].dwFlags = RIDEV_INPUTSink; devs[2].hwndTarget = this.Handle;

        RegisterRawInputDevices(devs, (uint)devs.Length, (uint)Marshal.SizeOf(typeof(RAWINPUTDEVICE)));
    }

    protected override void WndProc(ref Message m)
    {
        if (m.Msg == WM_INPUT)
        {
            uint dwSize = 0;
            uint headerSize = (uint)Marshal.SizeOf(typeof(RAWINPUTHEADER));
            GetRawInputData(m.LParam, RID_INPUT, IntPtr.Zero, ref dwSize, headerSize);
            if (dwSize > 0)
            {
                IntPtr buffer = Marshal.AllocHGlobal((int)dwSize);
                try
                {
                    if (GetRawInputData(m.LParam, RID_INPUT, buffer, ref dwSize, headerSize) == dwSize)
                    {
                        RAWINPUTHEADER hdr = (RAWINPUTHEADER)Marshal.PtrToStructure(buffer, typeof(RAWINPUTHEADER));
                        IntPtr dataPtr = new IntPtr(buffer.ToInt64() + headerSize);

                        if (hdr.dwType == 1) // RIM_TYPEKEYBOARD
                        {
                            RAWKEYBOARD kb = (RAWKEYBOARD)Marshal.PtrToStructure(dataPtr, typeof(RAWKEYBOARD));
                            int val = ((kb.Flags & 1) == 0) ? 1 : 0; // 1 = press, 0 = release
                            string linuxKey = MapVKeyToLinux(kb.VKey, kb.MakeCode);
                            Emit(string.Format("[TECLADO/MEDIA] VKey=0x{0:X2} Scan=0x{1:X2} ->  METHOD EV_KEY {2} {3}",
                                kb.VKey, kb.MakeCode, linuxKey, val));
                        }
                        else if (hdr.dwType == 0) // RIM_TYPEMOUSE
                        {
                            RAWMOUSE ms = (RAWMOUSE)Marshal.PtrToStructure(dataPtr, typeof(RAWMOUSE));
                            bool isAbsolute = (ms.usFlags & 1) != 0;

                            if ((ms.usButtonFlags & 0x0001) != 0) // Left button down
                            {
                                buttonDown = true;
                                accumX = 0; accumY = 0;
                                Emit("[RATO/TOQUE] Botão Pressionado -> METHOD EV_KEY BTN_MOUSE 1");
                            }
                            if (buttonDown && (ms.lLastX != 0 || ms.lLastY != 0))
                            {
                                accumX += ms.lLastX;
                                accumY += ms.lLastY;
                                if (isAbsolute)
                                {
                                    Emit(string.Format("[ABS] X={0} Y={1} -> METHOD EV_ABS ABS_X {0}  /  METHOD EV_ABS ABS_Y {1}", ms.lLastX, ms.lLastY));
                                }
                            }
                            if ((ms.usButtonFlags & 0x0002) != 0) // Left button up
                            {
                                buttonDown = false;
                                string dir = "Centro (OK)";
                                if (Math.Abs(accumX) > Math.Abs(accumY) && Math.Abs(accumX) > 10)
                                    dir = (accumX > 0) ? "Esquerda (BLE-M3 ABS_X 147)" : "Direita (BLE-M3 ABS_X 778)";
                                else if (Math.Abs(accumY) > 10)
                                    dir = (accumY > 0) ? "Cima (BLE-M3 ABS_Y 112)" : "Baixo (BLE-M3 ABS_Y 880)";

                                Emit(string.Format("[GESTO/CLIQUE] Soltou botão: DeltaX={0}, DeltaY={1} => Provável botão: {2}", accumX, accumY, dir));
                            }
                            if ((ms.usButtonFlags & 0x0004) != 0)
                                Emit("[RATO] Botão Direito -> METHOD EV_KEY BTN_RIGHT 1");
                            if ((ms.usButtonFlags & 0x0010) != 0)
                                Emit("[RATO] Botão Meio -> METHOD EV_KEY BTN_MIDDLE 1");
                        }
                        else if (hdr.dwType == 2) // RIM_TYPEHID (Consumer Control)
                        {
                            int count = Marshal.ReadInt32(dataPtr, 4);
                            int size = Marshal.ReadInt32(dataPtr, 0);
                            if (count > 0 && size >= 2)
                            {
                                byte b0 = Marshal.ReadByte(dataPtr, 8);
                                byte b1 = (size >= 2) ? Marshal.ReadByte(dataPtr, 9) : (byte)0;
                                byte b2 = (size >= 3) ? Marshal.ReadByte(dataPtr, 10) : (byte)0;
                                Emit(string.Format("[HID CONSUMER] Raw=[{0:X2} {1:X2} {2:X2}]", b0, b1, b2));
                            }
                        }
                    }
                }
                finally
                {
                    Marshal.FreeHGlobal(buffer);
                }
            }
        }
        base.WndProc(ref m);
    }

    private void Emit(string msg)
    {
        if (OnEventCaptured != null)
            OnEventCaptured(DateTime.Now.ToString("HH:mm:ss.fff") + "  " + msg);
    }

    private static string MapVKeyToLinux(ushort vk, ushort scan)
    {
        switch (vk)
        {
            case 0xAF: return "KEY_VOLUMEUP";
            case 0xAE: return "KEY_VOLUMEDOWN";
            case 0xAD: return "KEY_MUTE";
            case 0xB0: return "KEY_NEXTSONG";
            case 0xB1: return "KEY_PREVIOUSSONG";
            case 0xB2: return "KEY_STOPCD";
            case 0xB3: return "KEY_PLAYPAUSE";
            case 0x0D: return "KEY_ENTER";
            case 0x1B: return "KEY_ESC";
            case 0x20: return "KEY_SPACE";
            case 0x21: return "KEY_PAGEUP";
            case 0x22: return "KEY_PAGEDOWN";
            case 0x25: return "KEY_LEFT";
            case 0x26: return "KEY_UP";
            case 0x27: return "KEY_RIGHT";
            case 0x28: return "KEY_DOWN";
            case 0xAC: return "KEY_HOMEPAGE";
            case 0xA6: return "KEY_BACK";
            default:   return string.Format("0x{0:X2} /* VKey */", vk);
        }
    }
}
"@

Add-Type -TypeDefinition $csharp -ReferencedAssemblies System.Windows.Forms, System.Drawing

$form = New-Object RawInputCaptureWindow
$form.Text = "Kobo BTPT Plus - Captura de Eventos Bluetooth (Windows)"
$form.Size = New-Object System.Drawing.Size(780, 480)
$form.StartPosition = "CenterScreen"

$lbl = New-Object System.Windows.Forms.Label
$lbl.Text = "Pressiona os botões do comando Bluetooth emparelhado no Windows (clica dentro desta janela primeiro):"
$lbl.Dock = "Top"
$lbl.Height = 28
$lbl.Padding = New-Object System.Windows.Forms.Padding(8, 6, 8, 0)

$box = New-Object System.Windows.Forms.TextBox
$box.Multiline = $true
$box.ReadOnly = $true
$box.ScrollBars = "Vertical"
$box.Dock = "Fill"
$box.Font = New-Object System.Drawing.Font("Consolas", 10)
$box.BackColor = [System.Drawing.Color]::FromArgb(30, 30, 30)
$box.ForeColor = [System.Drawing.Color]::FromArgb(220, 220, 220)

$form.Controls.Add($box)
$form.Controls.Add($lbl)

$form.add_OnEventCaptured({
    param($line)
    $box.AppendText($line + [Environment]::NewLine)
})

Write-Host "Janela de captura aberta. Pressiona os botões do comando Bluetooth..." -ForegroundColor Cyan
[void]$form.ShowDialog()
