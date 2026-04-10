// =============================================================================
//  OmegaDomStreamer.cs -- cTrader Algo cBot
//
//  Reads real XAUUSD DOM sizes from inside cTrader (where real volumes ARE
//  available) and streams them to Omega via a local TCP socket on port 8765.
//
//  INSTALL:
//    1. Open cTrader Desktop on your Windows VPS (or Mac if running cTrader there)
//    2. Go to Automate -> New Bot -> paste this code -> Build -> Add Instance
//    3. Attach to XAUUSD M1 chart, Run
//    4. Omega connects to localhost:8765 and reads real DOM updates
//
//  PROTOCOL (newline-delimited JSON):
//    {"ts":1712345678901,"bids":[[4762.50,6000],[4762.00,20000]],"asks":[[4763.00,10000],[4763.50,2000]]}
//    ts   = Unix ms
//    bids = [[price, volumeInUnits], ...] best bid first
//    asks = [[price, volumeInUnits], ...] best ask first
//    volumeInUnits = actual lot units (e.g. 6000 = 6 lots at 1000 units/lot)
//
//  Omega receives this on a background thread and updates g_real_dom_gold.
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
            Print($"[OMEGA-DOM] Starting DOM streamer for {TargetSymbol} on port {Port}");

            // Subscribe to market depth
            _depth = MarketData.GetMarketDepth(TargetSymbol);
            _depth.Updated += OnDepthUpdated;

            // Start TCP listener on background thread
            _listenThread = new Thread(ListenLoop) { IsBackground = true, Name = "OmegaDomListener" };
            _listenThread.Start();

            Print($"[OMEGA-DOM] TCP server started on port {Port}. Waiting for Omega connection...");
        }

        private void ListenLoop()
        {
            try
            {
                _listener = new TcpListener(IPAddress.Loopback, Port);
                _listener.Start();
                Print($"[OMEGA-DOM] Listening on 127.0.0.1:{Port}");

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
                            Print($"[OMEGA-DOM] Client connected: {((IPEndPoint)client.Client.RemoteEndPoint).Address}");
                        }
                        else
                        {
                            Thread.Sleep(10);
                        }
                    }
                    catch (Exception ex)
                    {
                        if (_running) Print($"[OMEGA-DOM] Accept error: {ex.Message}");
                    }
                }
            }
            catch (Exception ex)
            {
                Print($"[OMEGA-DOM] Listener failed: {ex.Message}");
            }
        }

        private void OnDepthUpdated()
        {
            // Rate limit: don't flood Omega
            var now = DateTime.UtcNow;
            if ((now - _lastSent).TotalMilliseconds < MinUpdateMs) return;
            _lastSent = now;
            _updateCount++;

            // Build JSON message
            var sb = new StringBuilder(512);
            var ts = DateTimeOffset.UtcNow.ToUnixTimeMilliseconds();
            sb.Append("{"ts":");
            sb.Append(ts);
            sb.Append(","bids":[");

            var bidCount = 0;
            foreach (var entry in _depth.BidEntries)
            {
                if (bidCount >= MaxLevels) break;
                if (bidCount > 0) sb.Append(',');
                sb.Append('[');
                sb.Append(entry.Price.ToString("F5"));
                sb.Append(',');
                sb.Append((long)entry.VolumeInUnits);
                sb.Append(']');
                bidCount++;
            }

            sb.Append("],"asks":[");

            var askCount = 0;
            foreach (var entry in _depth.AskEntries)
            {
                if (askCount >= MaxLevels) break;
                if (askCount > 0) sb.Append(',');
                sb.Append('[');
                sb.Append(entry.Price.ToString("F5"));
                sb.Append(',');
                sb.Append((long)entry.VolumeInUnits);
                sb.Append(']');
                askCount++;
            }

            sb.Append("],"bid_levels":");
            sb.Append(bidCount);
            sb.Append(","ask_levels":");
            sb.Append(askCount);
            sb.Append(","seq":");
            sb.Append(_updateCount);
            sb.Append("}\n");

            var bytes = Encoding.UTF8.GetBytes(sb.ToString());

            // Send to all connected clients, remove dead ones
            var dead = new System.Collections.Generic.List<TcpClient>();
            foreach (var client in _clients)
            {
                try
                {
                    if (!client.Connected) { dead.Add(client); continue; }
                    var stream = client.GetStream();
                    if (stream.CanWrite)
                        stream.Write(bytes, 0, bytes.Length);
                }
                catch
                {
                    dead.Add(client);
                }
            }

            // Log every 1000 updates
            if (_updateCount % 1000 == 0)
                Print($"[OMEGA-DOM] {_updateCount} DOM updates sent | bids={bidCount} asks={askCount} | clients={_clients.Count - dead.Count}");
        }

        protected override void OnStop()
        {
            _running = false;
            try { _listener?.Stop(); } catch { }
            foreach (var c in _clients)
                try { c.Close(); } catch { }
            Print($"[OMEGA-DOM] Stopped. Total updates sent: {_updateCount}");
        }
    }
}
