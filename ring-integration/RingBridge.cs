using System;
using System.IO;
using System.Linq;
using System.Net;
using System.Text;
using System.Threading.Tasks;
using Windows.Devices.Bluetooth;
using Windows.Devices.Bluetooth.GenericAttributeProfile;
using Windows.Devices.Enumeration;
using Windows.Foundation;
using Windows.Storage.Streams;

class RingBridge
{
    const string Address = "http://127.0.0.1:8765/";
    const string StudioPage = "http://127.0.0.1:4173/ring-integration/";
    const string Ring = "BCL6039F5A";
    static int sequence;
    static Guid Uuid(string part) { return new Guid("bae8" + part + "-4f05-4503-8e65-3af1f7329d1f"); }

    static void Main(string[] args)
    {
        Console.OutputEncoding = Encoding.UTF8;
        try {
            if (args.Contains("--measure")) Console.WriteLine(Measure().GetAwaiter().GetResult());
            else if (args.Contains("--check-page")) {
                var server = Serve();
                var request = (HttpWebRequest)WebRequest.Create(Address);
                request.Proxy = null;
                request.AllowAutoRedirect = false;
                using (var response = (HttpWebResponse)request.GetResponse()) {
                    if (response.StatusCode != HttpStatusCode.Redirect ||
                        response.Headers[HttpResponseHeader.Location] != StudioPage)
                        throw new InvalidOperationException("戒指联动页面跳转检查失败。");
                }
                Console.WriteLine("网页桥接检查成功。");
            } else Serve().GetAwaiter().GetResult();
        } catch (Exception error) {
            Console.WriteLine(error.Message);
            if (args.Length == 0) { Console.WriteLine("按回车关闭。"); Console.ReadLine(); }
            Environment.ExitCode = 1;
        }
    }

    static async Task<T> Wait<T>(IAsyncOperation<T> operation, int milliseconds = 10000)
    {
        var task = operation.AsTask();
        if (await Task.WhenAny(task, Task.Delay(milliseconds)) != task) {
            operation.Cancel();
            throw new TimeoutException("Windows 蓝牙请求超时。");
        }
        return await task;
    }

    static async Task<string> Measure()
    {
        var paired = await Wait(DeviceInformation.FindAllAsync(
            BluetoothLEDevice.GetDeviceSelectorFromPairingState(true)));
        var match = paired.FirstOrDefault(device => device.Name == Ring);
        if (match == null) throw new InvalidOperationException("Windows 未找到 " + Ring + " 的配对记录。");
        using (var device = await Wait(BluetoothLEDevice.FromIdAsync(match.Id))) {
            if (device == null) throw new InvalidOperationException("Windows 无法打开戒指蓝牙数据接口。");
            var services = await Wait(device.GetGattServicesForUuidAsync(Uuid("0001")));
            if (services.Status != GattCommunicationStatus.Success || services.Services.Count == 0)
                throw new InvalidOperationException("无法访问戒指测量服务：" + services.Status);
            using (var service = services.Services[0]) {
                var commands = await Wait(service.GetCharacteristicsForUuidAsync(Uuid("0010")));
                var replies = await Wait(service.GetCharacteristicsForUuidAsync(Uuid("0011")));
                if (commands.Status != GattCommunicationStatus.Success || commands.Characteristics.Count == 0 ||
                    replies.Status != GattCommunicationStatus.Success || replies.Characteristics.Count == 0)
                    throw new InvalidOperationException("找不到测量数据接口。");
                var command = commands.Characteristics[0];
                var reply = replies.Characteristics[0];
                var result = new TaskCompletionSource<string>(TaskCreationOptions.RunContinuationsAsynchronously);
                byte id = (byte)(sequence = sequence % 255 + 1);
                var progress = 0;
                TypedEventHandler<GattCharacteristic, GattValueChangedEventArgs> handler = (sender, e) => {
                    try {
                        byte[] packet;
                        using (var reader = DataReader.FromBuffer(e.CharacteristicValue)) {
                            packet = new byte[reader.UnconsumedBufferLength];
                            reader.ReadBytes(packet);
                        }
                        if (packet.Length < 5 || packet[0] != 0 || packet[1] != id || packet[2] != 0x32) return;
                        if (packet[3] == 255) { progress = packet[4]; return; }
                        if (packet[3] != 0 || packet.Length < 9) return;
                        var state = packet[4];
                        if (state == 0 || state == 2 || state == 4) {
                            var message = state == 0 ? "戒指未检测到佩戴。" : state == 2
                                ? "请将戒指取下充电座再测量。" : "戒指正在进行其他测量。";
                            result.TrySetException(new InvalidOperationException(message));
                            return;
                        }
                        if (state != 3 || progress < 100) return;
                        int temp = (short)(packet[7] | (packet[8] << 8));
                        result.TrySetResult(string.Format("{{\"heart\":{0},\"oxygen\":{1},\"temperature\":{2}}}",
                            packet[5] > 0 && packet[5] < 255 ? packet[5].ToString() : "null",
                            packet[6] > 0 && packet[6] <= 100 ? packet[6].ToString() : "null",
                            temp > 0 && temp < 10000 ? (temp / 100.0).ToString("F2", System.Globalization.CultureInfo.InvariantCulture) : "null"));
                    } catch (Exception error) { result.TrySetException(error); }
                };
                reply.ValueChanged += handler;
                try {
                    var notify = await Wait(reply.WriteClientCharacteristicConfigurationDescriptorAsync(GattClientCharacteristicConfigurationDescriptorValue.Notify));
                    if (notify != GattCommunicationStatus.Success) throw new InvalidOperationException("订阅测量通知失败：" + notify);
                    using (var writer = new DataWriter()) {
                        writer.WriteBytes(new byte[] { 0, id, 0x32, 0, 30, 0, 0, 1 });
                        var status = await Wait(command.WriteValueAsync(writer.DetachBuffer(), GattWriteOption.WriteWithResponse));
                        if (status != GattCommunicationStatus.Success) throw new InvalidOperationException("发送测量指令失败：" + status);
                    }
                    if (await Task.WhenAny(result.Task, Task.Delay(45000)) != result.Task)
                        throw new TimeoutException("未收到完整读数。请检查戒指佩戴和蓝牙连接。");
                    return await result.Task;
                } finally { reply.ValueChanged -= handler; }
            }
        }
    }

    static async Task Serve()
    {
        var server = new System.Net.Sockets.TcpListener(IPAddress.Loopback, 8765);
        server.Start();
        Console.WriteLine("戒指测量服务已启动。运行 frontend/start.cmd 后，请打开 " + StudioPage);
        Console.WriteLine("保留这个窗口，关闭窗口即可停止服务。");
        while (true) {
            using (var client = await server.AcceptTcpClientAsync()) {
                try { Respond(client.GetStream()); }
                catch (IOException) { }
            }
        }
    }

    static void Respond(System.Net.Sockets.NetworkStream stream)
    {
        stream.ReadTimeout = 2000;
        var header = new StringBuilder();
        while (header.Length < 8192) {
            int value = stream.ReadByte();
            if (value < 0) return;
            header.Append((char)value);
            int n = header.Length;
            if (n >= 4 && header[n-4] == '\r' && header[n-3] == '\n' &&
                header[n-2] == '\r' && header[n-1] == '\n') break;
        }
        string[] lines = header.ToString().Split(new[] { "\r\n" }, StringSplitOptions.None);
        Func<string, bool> has = value => lines.Skip(1).Any(line => line.Equals(value, StringComparison.OrdinalIgnoreCase));
        if (!has("Host: 127.0.0.1:8765")) {
            Reply(stream, "404 Not Found", "text/plain", "找不到这个请求。");
        } else if (lines[0] == "GET / HTTP/1.1") {
            Reply(stream, "302 Found", "text/plain", "请从 Connect Studio 打开戒指联动页面。", StudioPage);
        } else if (lines[0] == "POST /measure HTTP/1.1" &&
                   has("Origin: " + Address.TrimEnd('/')) && has("X-Ring-Measure: 1")) {
            string result;
            try { result = Measure().GetAwaiter().GetResult(); }
            catch (Exception error) { Reply(stream, "503 Service Unavailable", "text/plain", error.Message); return; }
            Reply(stream, "200 OK", "application/json", result);
        } else {
            Reply(stream, "404 Not Found", "text/plain", "找不到这个请求。");
        }
    }

    static void Reply(Stream stream, string status, string type, string text, string location = null)
    {
        byte[] body = Encoding.UTF8.GetBytes(text);
        string locationHeader = location == null ? "" : "Location: " + location + "\r\n";
        byte[] header = Encoding.ASCII.GetBytes("HTTP/1.1 " + status + "\r\nContent-Type: " + type +
            "; charset=utf-8\r\nContent-Length: " + body.Length + "\r\n" + locationHeader +
            "Cache-Control: no-store\r\nConnection: close\r\n\r\n");
        stream.Write(header, 0, header.Length);
        stream.Write(body, 0, body.Length);
    }
}
