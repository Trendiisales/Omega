// =============================================================================
//  OmegaDomStreamer.cs -- cTrader Algo cBot
//  Reads real XAUUSD DOM sizes from cTrader and streams to Omega on port 8765.
//  Protocol: newline-delimited JSON per DOM update.
// =============================================================================

using System;
using System.Net;
using System.Net.Sockets;
using System.Text;
using System.Collections.Concurrent;
using System.Threading;
using cAlgo.API;
using cAlgo.API.Internals;

namespace cAlgo.Robots
{
    [Robot(TimeZone = TimeZones.UTC, AccessRights = AccessRights.FullAccess)]
    public class OmegaDomStreamer : Robot
    {
        [Parameter("Port", DefaultValue = 8765)]
        public int Port { get; set; }

        [Parameter("Symbol", DefaultValue = "XAUUSD")]
        public string TargetSymbol { get; set; }

        [Parameter("Max DOM Levels", DefaultValue = 20)]
        public int MaxLevels { get; set; }

        [Parameter("Min Update Interval Ms", DefaultValue = 10)]
        public int MinUpdateMs { get; set; }

        private MarketDepth _depth;
        private TcpListener _listener;
        private ConcurrentBag<TcpClient> _clients = new ConcurrentBag<TcpClient>();
        private Thread _listenThread;
        private volatile bool _running = true;
        private DateTime _lastSent = DateTime.MinValue;
        private long _updateCount = 0;

        protected override void OnStart()
        {
            Print("[OMEGA-DOM] Starting DOM streamer for " + TargetSymbol + " on port " + Port);
            _depth = MarketData.GetMarketDepth(TargetSymbol);
            _depth.Updated += OnDepthUpdated;
            _listenThread = new Thread(ListenLoop) { IsBackground = true, Name = "OmegaDomListener" };
            _listenThread.Start();
            Print("[OMEGA-DOM] TCP server started on port " + Port);
        }

        private void ListenLoop()
        {
            try
            {
                _listener = new TcpListener(IPAddress.Loopback, Port);
                _listener.Start();
                Print("[OMEGA-DOM] Listening on 127.0.0.1:" + Port);
                while (_running)
                {
                    try
                    {
                        if (_listener.Pending())
                        {
                            var client = _listener.AcceptTcpClient();
                            client.NoDelay = true;
                            client.SendTimeout = 1000;
                            _clients.Add(client);
                            Print("[OMEGA-DOM] Client connected");
                        }
                        else { Thread.Sleep(10); }
                    }
                    catch (Exception ex)
                    {
                        if (_running) Print("[OMEGA-DOM] Accept error: " + ex.Message);
                    }
                }
            }
            catch (Exception ex)
            {
                Print("[OMEGA-DOM] Listener failed: " + ex.Message);
            }
        }

        private void OnDepthUpdated()
        {
            var now = DateTime.UtcNow;
            if ((now - _lastSent).TotalMilliseconds < MinUpdateMs) return;
            _lastSent = now;
            _updateCount++;

            var sb = new StringBuilder(512);
            var ts = DateTimeOffset.UtcNow.ToUnixTimeMilliseconds();
            sb.Append("{" + (char)34 + "ts" + (char)34 + ":");
            sb.Append(ts);
            sb.Append("," + (char)34 + "bids" + (char)34 + ":[");

            var bidCount = 0;
            foreach (var entry in _depth.BidEntries)
            {
                if (bidCount >= MaxLevels) break;
                if (bidCount > 0) sb.Append(",");
                sb.Append("[");
                sb.Append(entry.Price.ToString("F5"));
                sb.Append(",");
                sb.Append((long)entry.VolumeInUnits);
                sb.Append("]");
                bidCount++;
            }

            sb.Append("]," + (char)34 + "asks" + (char)34 + ":[");

            var askCount = 0;
            foreach (var entry in _depth.AskEntries)
            {
                if (askCount >= MaxLevels) break;
                if (askCount > 0) sb.Append(",");
                sb.Append("[");
                sb.Append(entry.Price.ToString("F5"));
                sb.Append(",");
                sb.Append((long)entry.VolumeInUnits);
                sb.Append("]");
                askCount++;
            }

            sb.Append("]," + (char)34 + "bid_levels" + (char)34 + ":");
            sb.Append(bidCount);
            sb.Append("," + (char)34 + "ask_levels" + (char)34 + ":");
            sb.Append(askCount);
            sb.Append("," + (char)34 + "seq" + (char)34 + ":");
            sb.Append(_updateCount);
            sb.Append("}\n");

            var bytes = Encoding.UTF8.GetBytes(sb.ToString());
            var dead = new System.Collections.Generic.List<TcpClient>();
            foreach (var client in _clients)
            {
                try
                {
                    if (!client.Connected) { dead.Add(client); continue; }
                    var stream = client.GetStream();
                    if (stream.CanWrite) stream.Write(bytes, 0, bytes.Length);
                }
                catch { dead.Add(client); }
            }
            if (_updateCount % 1000 == 0)
                Print("[OMEGA-DOM] " + _updateCount + " updates | bids=" + bidCount + " asks=" + askCount);
        }

        protected override void OnStop()
        {
            _running = false;
            try { _listener?.Stop(); } catch { }
            foreach (var c in _clients) try { c.Close(); } catch { }
            Print("[OMEGA-DOM] Stopped. Total updates: " + _updateCount);
        }
    }
}
