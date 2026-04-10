// =============================================================================
//  OmegaDomStreamer.cs -- cTrader Algo cBot
//  Reads real XAUUSD DOM sizes from cTrader and streams to Omega on port 8765.
//  Protocol: newline-delimited JSON per DOM update.
//  Also logs DOM snapshots to C:\Omega\logs\dom_stream_YYYY-MM-DD.csv for backtesting.
// =============================================================================

using System;
using System.IO;
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

        public string TargetSymbol { get { return "XAUUSD"; } }

        [Parameter("Max DOM Levels", DefaultValue = 20)]
        public int MaxLevels { get; set; }

        [Parameter("Min Update Interval Ms", DefaultValue = 10)]
        public int MinUpdateMs { get; set; }

        [Parameter("Log To CSV", DefaultValue = true)]
        public bool LogToCsv { get; set; }

        private MarketDepth _depth;
        private TcpListener _listener;
        private ConcurrentBag<TcpClient> _clients = new ConcurrentBag<TcpClient>();
        private Thread _listenThread;
        private volatile bool _running = true;
        private DateTime _lastSent = DateTime.MinValue;
        private long _updateCount = 0;

        // CSV logging
        private StreamWriter _csvWriter;
        private string _csvPath;
        private string _csvDate;

        protected override void OnStart()
        {
            Print("[OMEGA-DOM] Starting DOM streamer for " + TargetSymbol + " on port " + Port);
            _depth = MarketData.GetMarketDepth(TargetSymbol);
            _depth.Updated += OnDepthUpdated;
            _listenThread = new Thread(ListenLoop) { IsBackground = true, Name = "OmegaDomListener" };
            _listenThread.Start();
            if (LogToCsv) OpenCsvForToday();
            Print("[OMEGA-DOM] TCP server started on port " + Port);
        }

        private void OpenCsvForToday()
        {
            try
            {
                _csvDate = DateTime.UtcNow.ToString("yyyy-MM-dd");
                // Try C:\Omega\logs\ first, fall back to user temp
                var primaryPath = @"C:\Omega\logs\dom_stream_" + _csvDate + ".csv";
                var fallbackPath = System.IO.Path.Combine(
                    Environment.GetFolderPath(Environment.SpecialFolder.UserProfile),
                    "dom_stream_" + _csvDate + ".csv");
                _csvPath = primaryPath;
                try { var testFile = File.Create(primaryPath); testFile.Close(); File.Delete(primaryPath); }
                catch { _csvPath = fallbackPath; Print("[OMEGA-DOM] Cannot write to logs dir, using: " + fallbackPath); }
                bool exists = File.Exists(_csvPath);
                _csvWriter = new StreamWriter(_csvPath, append: true, encoding: Encoding.UTF8);
                _csvWriter.AutoFlush = true;
                if (!exists)
                    _csvWriter.WriteLine("ts_ms,bid_imb,top5_bid_vol,top5_ask_vol,best_bid_px,best_ask_px,bid_levels,ask_levels");
                Print("[OMEGA-DOM] CSV logging to " + _csvPath);
            }
            catch (Exception ex)
            {
                Print("[OMEGA-DOM] CSV open failed: " + ex.Message);
                _csvWriter = null;
            }
        }

        private void RotateCsvIfNeeded()
        {
            if (_csvWriter == null) return;
            var today = DateTime.UtcNow.ToString("yyyy-MM-dd");
            if (today == _csvDate) return;
            try { _csvWriter.Close(); } catch { }
            _csvDate = today;
            // Try C:\Omega\logs\ first, fall back to user temp
                var primaryPath = @"C:\Omega\logs\dom_stream_" + _csvDate + ".csv";
                var fallbackPath = System.IO.Path.Combine(
                    Environment.GetFolderPath(Environment.SpecialFolder.UserProfile),
                    "dom_stream_" + _csvDate + ".csv");
                _csvPath = primaryPath;
                try { var testFile = File.Create(primaryPath); testFile.Close(); File.Delete(primaryPath); }
                catch { _csvPath = fallbackPath; Print("[OMEGA-DOM] Cannot write to logs dir, using: " + fallbackPath); }
            _csvWriter = new StreamWriter(_csvPath, append: false, encoding: Encoding.UTF8);
            _csvWriter.AutoFlush = true;
            _csvWriter.WriteLine("ts_ms,bid_imb,top5_bid_vol,top5_ask_vol,best_bid_px,best_ask_px,bid_levels,ask_levels");
        }

        private void ListenLoop()
        {
            try
            {
                _listener = new TcpListener(IPAddress.Loopback, Port);
                _listener.Server.SetSocketOption(System.Net.Sockets.SocketOptionLevel.Socket, System.Net.Sockets.SocketOptionName.ReuseAddress, true);
                _listener.Server.SetSocketOption(System.Net.Sockets.SocketOptionLevel.Socket, System.Net.Sockets.SocketOptionName.ExclusiveAddressUse, false);
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

            // Build bid/ask arrays
            var bids = new System.Collections.Generic.List<double[]>();
            var asks = new System.Collections.Generic.List<double[]>();
            foreach (var e in _depth.BidEntries) { if (bids.Count >= MaxLevels) break; bids.Add(new[]{e.Price, (double)(long)e.VolumeInUnits}); }
            foreach (var e in _depth.AskEntries) { if (asks.Count >= MaxLevels) break; asks.Add(new[]{e.Price, (double)(long)e.VolumeInUnits}); }

            int bidCount = bids.Count;
            int askCount = asks.Count;

            // Compute top-5 imbalance for CSV
            double bidVol5 = 0, askVol5 = 0;
            for (int i = 0; i < Math.Min(5, bidCount); i++) bidVol5 += bids[i][1];
            for (int i = 0; i < Math.Min(5, askCount); i++) askVol5 += asks[i][1];
            double total5 = bidVol5 + askVol5;
            double imb5 = total5 > 0 ? bidVol5 / total5 : 0.5;

            // Write CSV
            if (LogToCsv && _csvWriter != null)
            {
                RotateCsvIfNeeded();
                var ts = DateTimeOffset.UtcNow.ToUnixTimeMilliseconds();
                double bestBid = bidCount > 0 ? bids[0][0] : 0;
                double bestAsk = askCount > 0 ? asks[0][0] : 0;
                _csvWriter.WriteLine(
                    ts + "," +
                    imb5.ToString("F4") + "," +
                    bidVol5.ToString("F0") + "," +
                    askVol5.ToString("F0") + "," +
                    bestBid.ToString("F5") + "," +
                    bestAsk.ToString("F5") + "," +
                    bidCount + "," +
                    askCount
                );
            }

            // Build JSON for TCP stream
            var sb = new StringBuilder(512);
            var tsJson = DateTimeOffset.UtcNow.ToUnixTimeMilliseconds();
            sb.Append("{\"ts\":"); sb.Append(tsJson);
            sb.Append(",\"bids\":[");
            for (int i = 0; i < bidCount; i++)
            {
                if (i > 0) sb.Append(",");
                sb.Append("["); sb.Append(bids[i][0].ToString("F5")); sb.Append(","); sb.Append((long)bids[i][1]); sb.Append("]");
            }
            sb.Append("],\"asks\":[");
            for (int i = 0; i < askCount; i++)
            {
                if (i > 0) sb.Append(",");
                sb.Append("["); sb.Append(asks[i][0].ToString("F5")); sb.Append(","); sb.Append((long)asks[i][1]); sb.Append("]");
            }
            sb.Append("],\"bid_levels\":"); sb.Append(bidCount);
            sb.Append(",\"ask_levels\":"); sb.Append(askCount);
            sb.Append(",\"imb5\":"); sb.Append(imb5.ToString("F4"));
            sb.Append(",\"seq\":"); sb.Append(_updateCount);
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
                Print("[OMEGA-DOM] " + _updateCount + " updates | bids=" + bidCount + " asks=" + askCount + " imb5=" + imb5.ToString("F3"));
        }

        protected override void OnStop()
        {
            _running = false;
            try { _listener?.Stop(); } catch { }
            foreach (var c in _clients) try { c.Close(); } catch { }
            try { if (_csvWriter != null) { _csvWriter.Flush(); _csvWriter.Close(); } } catch { }
            Print("[OMEGA-DOM] Stopped. Total updates: " + _updateCount);
        }
    }
}
