﻿using System;
using System.Collections.Concurrent;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Net.Sockets;
using System.Text;
using System.Threading;
using System.Threading.Tasks;

namespace minisphere.Gdk.Duktape
{
    enum DValue
    {
        EOM,
        REQ,
        REP,
        ERR,
        NFY,
        Unused,
        Undefined,
        Null,
        Object,
        HeapPtr,
        Pointer,
        Lightfunc,
    }

    enum Request
    {
        BasicInfo = 0x10,
        TriggerStatus = 0x11,
        Pause = 0x12,
        Resume = 0x13,
        StepInto = 0x14,
        StepOver = 0x15,
        StepOut = 0x16,
        ListBreak = 0x17,
        AddBreak = 0x18,
        DelBreak = 0x19,
        GetVar = 0x1A,
        PutVar = 0x1B,
        GetCallStack = 0x1C,
        GetLocals = 0x1D,
        Eval = 0x1E,
        Detach = 0x1F,
        DumpHeap = 0x20,
        GetByteCode = 0x21,
        AppRequest = 0x22,
        InspectHeapObject = 0x23,
    }

    enum Notify
    {
        Status = 0x01,
        Print = 0x02,
        Alert = 0x03,
        Log = 0x04,
        Throw = 0x05,
        Detaching = 0x06,
        AppNotify = 0x07,
    }

    enum AppRequest
    {
        GameInfo = 0x01,
        Source = 0x02,
    }

    enum AppNotify
    {
        DebugPrint = 0x01,
    }

    class ErrorThrownEventArgs : EventArgs
    {
        public ErrorThrownEventArgs(string message, string filename, int lineNumber, bool isFatal)
        {
            Message = message;
            FileName = filename;
            LineNumber = lineNumber;
            IsFatal = isFatal;
        }

        /// <summary>
        /// Gets whether the error was fatal, i.e. unhandled.
        /// </summary>
        public bool IsFatal { get; private set; }

        /// <summary>
        /// Gets the string representation of the thrown value.
        /// </summary>
        public string Message { get; private set; }
        
        /// <summary>
        /// Gets the filename of the script throwing the error.
        /// </summary>
        public string FileName { get; private set; }

        /// <summary>
        /// Gets the line number where the error was thrown.
        /// </summary>
        public int LineNumber { get; private set; }
    }

    class TraceEventArgs : EventArgs
    {
        public TraceEventArgs(string text)
        {
            Text = text;
        }

        public string Text { get; private set; }
    }
    
    /// <summary>
    /// Allows control of a Duktape debug target over TCP.
    /// </summary>
    class Inferior : IDisposable
    {
        private TcpClient tcp = new TcpClient() { NoDelay = true };
        private Thread messenger = null;
        private object replyLock = new object();
        private Queue<dynamic[]> requests = new Queue<dynamic[]>();
        private Dictionary<dynamic[], dynamic[]> replies = new Dictionary<dynamic[], dynamic[]>();

        /// <summary>
        /// Constructs a DuktapeClient object used for communicating with a
        /// Duktape debuggee over TCP.
        /// </summary>
        public Inferior()
        {
        }

        ~Inferior()
        {
            Dispose();
        }

        /// <summary>
        /// Releases all resources used by the DuktapeClient object.
        /// </summary>
        public void Dispose()
        {
            messenger?.Abort();
            Alert = null;
            Attached = null;
            Detached = null;
            ErrorThrown = null;
            Print = null;
            Status = null;
            tcp.Close();
        }

        /// <summary>
        /// Fires when a script calls alert().
        /// </summary>
        public event EventHandler<TraceEventArgs> Alert;

        /// <summary>
        /// Fires when the debugger is attached to a target.
        /// </summary>
        public event EventHandler Attached;

        /// <summary>
        /// Fires when the debugger is detached from a target.
        /// </summary>
        public event EventHandler Detached;

        /// <summary>
        /// Fires when an error is thrown by JS code.
        /// </summary>
        public event EventHandler<ErrorThrownEventArgs> ErrorThrown;
        
        /// <summary>
        /// Fires when a script calls print().
        /// </summary>
        public event EventHandler<TraceEventArgs> Print;

        /// <summary>
        /// Fires when execution status (code position, etc.) has changed.
        /// </summary>
        public event EventHandler Status;
        
        /// <summary>
        /// Gets the identification string reported in the handshake.
        /// </summary>
        public string TargetID { get; private set; }

        /// <summary>
        /// Gets the version identification of the Duktape host.
        /// </summary>
        public string Version { get; private set; }
        
        /// <summary>
        /// Gets the filename reported in the last status update.
        /// </summary>
        public string FileName { get; private set; }

        /// <summary>
        /// Gets the line number being executed as of the last status update.
        /// </summary>
        public int LineNumber { get; private set; }

        /// <summary>
        /// Gets whether the target is currently executing code.
        /// </summary>
        public bool Running { get; private set; }

        public async Task Connect(string hostname, int port)
        {
            // connect to Duktape debug server
            await tcp.ConnectAsync(hostname, port);
            string line = "";
            await Task.Run(() =>
            {
                byte[] buffer = new byte[1];
                while (buffer[0] != '\n')
                {
                    tcp.Client.ReceiveAll(buffer);
                    line += (char)buffer[0];
                }
            });
            string[] handshake = line.Trim().Split(new[] { ' ' }, 4);
            int debuggerVersion = int.Parse(handshake[0]);
            if (debuggerVersion != 1)
                throw new NotSupportedException("Error communicating with debug server");

            Version = handshake[2];
            TargetID = handshake[3];

            // start the communication thread
            messenger = new Thread(ProcessMessages) { IsBackground = true };
            messenger.Start();
            Attached?.Invoke(this, EventArgs.Empty);
        }

        /// <summary>
        /// Sets a breakpoint. Execution will pause automatically if the breakpoint is hit.
        /// </summary>
        /// <param name="filename">The filename in which to place the breakpoint.</param>
        /// <param name="lineNumber">The line number of the breakpoint.</param>
        /// <returns>The index assigned to the breakpoint by Duktape.</returns>
        public async Task<int> AddBreak(string filename, int lineNumber)
        {
            var reply = await Converse(DValue.REQ, Request.AddBreak, filename, lineNumber);
            return reply[1];
        }

        /// <summary>
        /// Clears the breakpoint with the specified index.
        /// </summary>
        /// <param name="index">The index of the breakpoint to clear, as returned by AddBreak.</param>
        /// <returns></returns>
        public async Task DelBreak(int index)
        {
            await Converse(DValue.REQ, 0x19, index);
        }
        
        /// <summary>
        /// Requests that Duktape end the debug session.
        /// </summary>
        /// <returns></returns>
        public async Task Detach()
        {
            if (messenger == null)
                return;
            await Converse(DValue.REQ, 0x1F);
            await Task.Run(() => messenger.Join());
            tcp.Client.Disconnect(true);
            Detached?.Invoke(this, EventArgs.Empty);
        }

        /// <summary>
        /// Evaluates a JS expression and returns the JX-encoded result.
        /// </summary>
        /// <param name="expression">The expression or statement to evaluate.</param>
        /// <param name="stackOffset">The point in the stack to do the eval. -1 is active call, -2 the caller, etc..</param>
        /// <returns>The JX-encoded value produced by the expression.</returns>
        public async Task<string> Eval(string expression, int stackOffset = -1)
        {
            var code = string.Format(
                @"(function() {{ try {{ return Duktape.enc('jx', eval(""{0}""), null, 3); }} catch (e) {{ return e.toString(); }} }}).call(this);",
                expression.Replace(@"\", @"\\").Replace(@"""", @"\"""));
            var reply = await Converse(DValue.REQ, Request.Eval, code, stackOffset);
            return reply[2];
        }

        /// <summary>
        /// Gets a list of function calls currently on the stack. Note that Duktape
        /// supports tail return, so this may not reflect all active calls.
        /// </summary>
        /// <returns>
        /// An array of 3-tuples naming the function, filename and current line number
        /// of each function call on the stack, from top to the bottom.
        /// </returns>
        public async Task<Tuple<string, string, int>[]> GetCallStack()
        {
            var reply = await Converse(DValue.REQ, Request.GetCallStack);
            var stack = new List<Tuple<string, string, int>>();
            int count = (reply.Length - 1) / 4;
            for (int i = 0; i < count; ++i)
            {
                string filename = reply[1 + i * 4];
                string functionName = reply[2 + i * 4];
                int lineNumber = reply[3 + i * 4];
                int pc = reply[4 + i * 4];
                if (pc == 0)
                    filename = "(system call)";
                stack.Add(Tuple.Create(functionName, filename, lineNumber));
            }
            return stack.ToArray();
        }

        /// <summary>
        /// Gets a list of local values and their values. Note that objects
        /// are not evaluated and are listed simply as "{...}".
        /// </summary>
        /// <param name="stackOffset">The call stack offset to get locals for, -1 being the current activation.</param>
        /// <returns></returns>
        public async Task<IReadOnlyDictionary<string, string>> GetLocals(int stackOffset = -1)
        {
            var reply = await Converse(DValue.REQ, Request.GetLocals, stackOffset);
            var variables = new Dictionary<string, string>();
            int count = (reply.Length - 1) / 2;
            for (int i = 0; i < count; ++i)
            {
                string name = reply[1 + i * 2].ToString();
                dynamic value = reply[2 + i * 2];
                string friendlyValue = value.Equals(DValue.Object) ? "Object { ... }"
                    : value.Equals(DValue.Undefined) ? "undefined"
                    : value is bool ? value ? "true" : "false"
                    : value is int ? value.ToString()
                    : value is double ? value.ToString()
                    : value is string ? string.Format("\"{0}\"", value.Replace(@"""", @"\"""))
                    : await Eval(name);
                variables.Add(name, friendlyValue);
            }
            return variables;
        }

        /// <summary>
        /// Gets a list of currently set breakpoints.
        /// </summary>
        /// <returns>
        /// An array of 2-tuples specifying the location of each breakpoint
        /// as a filename/line number pair
        /// </returns>
        public async Task<Tuple<string, int>[]> ListBreak()
        {
            var reply = await Converse(DValue.REQ, Request.ListBreak);
            var count = (reply.Length - 1) / 2;
            List<Tuple<string, int>> list = new List<Tuple<string, int>>();
            for (int i = 0; i < count; ++i)
            {
                var breakpoint = Tuple.Create(reply[1 + i * 2], reply[2 + i * 2]);
                list.Add(breakpoint);
            }
            return list.ToArray();
        }

        /// <summary>
        /// Requests Duktape to pause execution and break into the debugger.
        /// This may take a second or so to register.
        /// </summary>
        /// <returns></returns>
        public async Task Pause()
        {
            await Converse(DValue.REQ, Request.Pause);
        }

        /// <summary>
        /// Resumes normal program execution.
        /// </summary>
        /// <returns></returns>
        public async Task Resume()
        {
            await Converse(DValue.REQ, Request.Resume);
        }

        /// <summary>
        /// Executes the next line of code. If a function is called, the debugger
        /// will break at the first statement in that function.
        /// </summary>
        /// <returns></returns>
        public async Task StepInto()
        {
            await Converse(DValue.REQ, Request.StepInto);
        }

        /// <summary>
        /// Resumes normal execution until the current function returns.
        /// </summary>
        /// <returns></returns>
        public async Task StepOut()
        {
            await Converse(DValue.REQ, Request.StepOut);
        }

        /// <summary>
        /// Executes the next line of code.
        /// </summary>
        /// <returns></returns>
        public async Task StepOver()
        {
            await Converse(DValue.REQ, Request.StepOut);
        }

        private async Task<dynamic[]> Converse(params dynamic[] values)
        {
            lock (replyLock) requests.Enqueue(values);
            foreach (dynamic value in values)
            {
                SendValue(value);
            }
            SendValue(DValue.EOM);
            return await Task.Run(() =>
            {
                while (true)
                {
                    lock (replyLock)
                    {
                        if (replies.ContainsKey(values))
                        {
                            var reply = replies[values];
                            replies.Remove(values);
                            return reply;
                        }
                    }
                    Thread.Sleep(1);
                }
            });
        }

        private dynamic[] ReceiveMessage()
        {
            List<dynamic> message = new List<dynamic>();
            dynamic value;
            do
            {
                if ((value = ReceiveValue()) == null)
                    return null;
                message.Add(value);
            } while (!value.Equals(DValue.EOM));
            return message.ToArray();
        }

        private dynamic ReceiveValue()
        {
            byte[] bytes;
            int length = -1;
            Encoding utf8 = new UTF8Encoding(false);

            if (!tcp.Client.ReceiveAll(bytes = new byte[1]))
                return null;
            if (bytes[0] >= 0x60 && bytes[0] < 0x80)
            {
                length = bytes[0] - 0x60;
                if (!tcp.Client.ReceiveAll(bytes = new byte[length]))
                    return null;
                return utf8.GetString(bytes);
            }
            else if (bytes[0] >= 0x80 && bytes[0] < 0xC0)
            {
                return bytes[0] - 0x80;
            }
            else if (bytes[0] >= 0xC0)
            {
                Array.Resize(ref bytes, 2);
                if (tcp.Client.Receive(bytes, 1, 1, SocketFlags.None) == 0)
                    return null;
                return ((bytes[0] - 0xC0) << 8) + bytes[1];
            }
            else
            {
                switch (bytes[0])
                {
                    case 0x00: return DValue.EOM;
                    case 0x01: return DValue.REQ;
                    case 0x02: return DValue.REP;
                    case 0x03: return DValue.ERR;
                    case 0x04: return DValue.NFY;
                    case 0x10: // 32-bit integer
                        if (!tcp.Client.ReceiveAll(bytes = new byte[4]))
                            return null;
                        return (bytes[0] << 24) + (bytes[1] << 16) + (bytes[2] << 8) + bytes[3];
                    case 0x11: // string with 32-bit length
                        if (!tcp.Client.ReceiveAll(bytes = new byte[4]))
                            return null;
                        length = (bytes[0] << 24) + (bytes[1] << 16) + (bytes[2] << 8) + bytes[3];
                        if (!tcp.Client.ReceiveAll(bytes = new byte[length]))
                            return null;
                        return utf8.GetString(bytes);
                    case 0x12: // string with 16-bit length
                        if (!tcp.Client.ReceiveAll(bytes = new byte[2]))
                            return null;
                        length = (bytes[0] << 8) + bytes[1];
                        if (!tcp.Client.ReceiveAll(bytes = new byte[length]))
                            return null;
                        return utf8.GetString(bytes);
                    case 0x13: // buffer with 32-bit length
                        if (!tcp.Client.ReceiveAll(bytes = new byte[4]))
                            return null;
                        length = (bytes[0] << 24) + (bytes[1] << 16) + (bytes[2] << 8) + bytes[3];
                        if (!tcp.Client.ReceiveAll(bytes = new byte[length]))
                            return null;
                        return bytes;
                    case 0x14: // buffer with 16-bit length
                        if (!tcp.Client.ReceiveAll(bytes = new byte[2]))
                            return null;
                        length = (bytes[0] << 8) + bytes[1];
                        if (!tcp.Client.ReceiveAll(bytes = new byte[length]))
                            return null;
                        return bytes;
                    case 0x15: return DValue.Unused;
                    case 0x16: return DValue.Undefined;
                    case 0x17: return DValue.Null;
                    case 0x18: return true;
                    case 0x19: return false;
                    case 0x1A: // IEEE double
                        if (!tcp.Client.ReceiveAll(bytes = new byte[8]))
                            return null;
                        if (BitConverter.IsLittleEndian)
                            Array.Reverse(bytes);
                        return BitConverter.ToDouble(bytes, 0);
                    case 0x1B: // JS object
                        tcp.Client.ReceiveAll(bytes = new byte[2]);
                        tcp.Client.ReceiveAll(new byte[bytes[1]]);
                        return DValue.Object;
                    case 0x1C: // pointer
                        tcp.Client.ReceiveAll(bytes = new byte[1]);
                        tcp.Client.ReceiveAll(new byte[bytes[0]]);
                        return DValue.Pointer;
                    case 0x1D: // Duktape lightfunc
                        tcp.Client.ReceiveAll(bytes = new byte[3]);
                        tcp.Client.ReceiveAll(new byte[bytes[2]]);
                        return DValue.Lightfunc;
                    case 0x1E: // Duktape heap pointer
                        tcp.Client.ReceiveAll(bytes = new byte[1]);
                        tcp.Client.ReceiveAll(new byte[bytes[0]]);
                        return DValue.HeapPtr;
                    default:
                        return DValue.EOM;
                }
            }
        }
        
        private void SendValue(DValue value)
        {
            switch (value)
            {
                case DValue.EOM: tcp.Client.Send(new byte[1] { 0x00 }); break;
                case DValue.REQ: tcp.Client.Send(new byte[1] { 0x01 }); break;
                case DValue.REP: tcp.Client.Send(new byte[1] { 0x02 }); break;
                case DValue.ERR: tcp.Client.Send(new byte[1] { 0x03 }); break;
                case DValue.NFY: tcp.Client.Send(new byte[1] { 0x04 }); break;
                case DValue.Unused: tcp.Client.Send(new byte[1] { 0x15 }); break;
                case DValue.Undefined: tcp.Client.Send(new byte[1] { 0x16 }); break;
                case DValue.Null: tcp.Client.Send(new byte[1] { 0x17 }); break;
            }
        }

        private void SendValue(Request value)
        {
            SendValue((int)value);
        }

        private void SendValue(bool value)
        {
            tcp.Client.Send(new byte[] {
                (byte)(value ? 0x18 : 0x19)
            });
        }

        private void SendValue(int value)
        {
            if (value >= 0 && value < 64)
                tcp.Client.Send(new byte[] { (byte)(0x80 + value) });
            else if (value >= 0 && value < 16384)
            {
                tcp.Client.Send(new byte[] {
                    (byte)(0xC0 + (value >> 8 & 0xFF)),
                    (byte)(value & 0xFF)
                });
            }
            else
            {
                tcp.Client.Send(new byte[] { 0x10 });
                tcp.Client.Send(new byte[] {
                    (byte)(value >> 24 & 0xFF),
                    (byte)(value >> 16 & 0xFF),
                    (byte)(value >> 8 & 0xFF),
                    (byte)(value & 0xFF)
                });
            }
        }

        private void SendValue(string value)
        {
            var utf8 = new UTF8Encoding(false);
            byte[] stringBytes = utf8.GetBytes(value);

            tcp.Client.Send(new byte[] { 0x11 });
            tcp.Client.Send(new byte[]
            {
                (byte)(stringBytes.Length >> 24 & 0xFF),
                (byte)(stringBytes.Length >> 16 & 0xFF),
                (byte)(stringBytes.Length >> 8 & 0xFF),
                (byte)(stringBytes.Length & 0xFF)
            });
            tcp.Client.Send(stringBytes);
        }

        private void ProcessMessages()
        {
            while (true)
            {
                dynamic[] message = ReceiveMessage();
                if (message == null)
                {
                    // if ReceiveMessage() returns null, detach.
                    tcp.Client.Disconnect(true);
                    Detached?.Invoke(this, EventArgs.Empty);
                    return;
                }
                if (message[0] == DValue.NFY)
                {
                    switch ((Notify)message[1])
                    {
                        case Notify.Status:
                            FileName = message[3];
                            LineNumber = message[5];
                            Running = message[2] == 0;
                            Status?.Invoke(this, EventArgs.Empty);
                            break;
                        case Notify.Print:
                            Print?.Invoke(this, new TraceEventArgs("print: " + message[2]));
                            break;
                        case Notify.Alert:
                            Alert?.Invoke(this, new TraceEventArgs(message[2]));
                            break;
                        case Notify.Throw:
                            ErrorThrown?.Invoke(this, new ErrorThrownEventArgs(
                                message[3], message[4], message[5],
                                message[2] != 0));
                            break;
                        case Notify.AppNotify:
                            switch ((AppNotify)message[2]) {
                                case AppNotify.DebugPrint:
                                    Print?.Invoke(this, new TraceEventArgs("t: " + message[3]));
                                    break;
                            }
                            break;
                    }
                }
                else if (message[0] == DValue.REP || message[0] == DValue.ERR)
                {
                    lock (replyLock)
                    {
                        dynamic[] request = requests.Dequeue();
                        replies.Add(request, message.Take(message.Length - 1).ToArray());
                    }
                }
            }
        }
    }
}
