using System;
using System.Text;

public class CPHInline
{
    // Must match EldenChaos.dll
    const int GameUdpPort = 22666;

    // Must match the Voice Alias NAME in Speaker.bot (Settings → Voice Aliases), not the Windows voice name.
    // Set to "" to skip CPH.TtsSpeak and use a Speak sub-action instead.
    const string TtsVoiceAlias = "Zira";

    static readonly string[] EventKinds = { "heal", "dogs", "rats", "hawks", "stat" };
    static readonly string[] Stats =
    {
        "vigor", "mind", "endurance", "strength",
        "dexterity", "intelligence", "faith", "arcane"
    };

    static readonly Random Rng = new Random();

    public bool Execute()
    {
        string user = GetUserName();
        var roll = RollEvent(user);

        // Streamer.bot built-in UDP (no System.Net needed)
        CPH.BroadcastUdp(GameUdpPort, roll.Json);

        // OBS overlay listens on Streamer.bot Websocket Server
        CPH.WebsocketBroadcastJson(roll.OverlayJson);

        CPH.SetArgument("chaosMessage", roll.Message);
        CPH.SetArgument("chaosKind", roll.Kind);
        CPH.SetArgument("chaosJson", roll.Json);

        try
        {
            if (!string.IsNullOrWhiteSpace(TtsVoiceAlias))
            {
                CPH.TtsSpeak(TtsVoiceAlias, roll.Message, false);
            }
        }
        catch (Exception ex)
        {
            CPH.LogWarn("EldenChaos TTS skipped: " + ex.Message);
        }

        CPH.LogInfo("EldenChaos: " + roll.Message);
        return true;
    }

    string GetUserName()
    {
        try
        {
            if (CPH.TryGetArg("userName", out string userName) && !string.IsNullOrWhiteSpace(userName))
                return userName;
        }
        catch { }

        try
        {
            if (CPH.TryGetArg("user", out string user) && !string.IsNullOrWhiteSpace(user))
                return user;
        }
        catch { }

        return "Someone";
    }

    static RollResult RollEvent(string user)
    {
        string kind = EventKinds[Rng.Next(EventKinds.Length)];
        string escUser = Escape(user);

        if (kind == "heal")
        {
            string message = user + " healed the Tarnished";
            string json = "{\"cmd\":\"heal\",\"user\":\"" + escUser + "\",\"message\":\"" + Escape(message) + "\"}";
            string overlay = OverlayJson(user, "heal", message, null, null, null, null);
            return new RollResult("heal", message, json, overlay);
        }

        if (kind == "dogs" || kind == "rats" || kind == "hawks")
        {
            string message = user + " spawned 3 " + kind;
            string json =
                "{\"cmd\":\"spawn\",\"enemy\":\"" + kind + "\",\"count\":3,\"user\":\"" + escUser +
                "\",\"message\":\"" + Escape(message) + "\"}";
            string overlay = OverlayJson(user, "spawn", message, kind, 3, null, null);
            return new RollResult("spawn", message, json, overlay);
        }

        // Random stat, then random +5 / -5
        string stat = Stats[Rng.Next(Stats.Length)];
        int delta = Rng.Next(2) == 0 ? 5 : -5;
        string sign = delta > 0 ? ("+" + delta) : delta.ToString();
        string pretty = char.ToUpper(stat[0]) + stat.Substring(1);
        string statMessage = user + " gave " + sign + " " + pretty;
        string statJson =
            "{\"cmd\":\"stat\",\"stat\":\"" + stat + "\",\"delta\":" + delta +
            ",\"user\":\"" + escUser + "\",\"message\":\"" + Escape(statMessage) + "\"}";
        string statOverlay = OverlayJson(user, "stat", statMessage, null, null, stat, delta);
        return new RollResult("stat", statMessage, statJson, statOverlay);
    }

    static string OverlayJson(string user, string eventKind, string message, string enemy, int? count, string stat, int? delta)
    {
        var sb = new StringBuilder();
        sb.Append("{\"source\":\"ockas-elden-chaos\",\"type\":\"chaos\"");
        sb.Append(",\"user\":\"").Append(Escape(user)).Append("\"");
        sb.Append(",\"eventKind\":\"").Append(Escape(eventKind)).Append("\"");
        sb.Append(",\"message\":\"").Append(Escape(message)).Append("\"");
        if (enemy != null) sb.Append(",\"enemy\":\"").Append(Escape(enemy)).Append("\"");
        if (count.HasValue) sb.Append(",\"count\":").Append(count.Value);
        if (stat != null) sb.Append(",\"stat\":\"").Append(Escape(stat)).Append("\"");
        if (delta.HasValue) sb.Append(",\"delta\":").Append(delta.Value);
        sb.Append("}");
        return sb.ToString();
    }

    static string Escape(string s)
    {
        if (string.IsNullOrEmpty(s)) return "";
        return s.Replace("\\", "\\\\").Replace("\"", "\\\"");
    }

    class RollResult
    {
        public string Kind;
        public string Message;
        public string Json;
        public string OverlayJson;

        public RollResult(string kind, string message, string json, string overlayJson)
        {
            Kind = kind;
            Message = message;
            Json = json;
            OverlayJson = overlayJson;
        }
    }
}
