using System;
using System.Text;

/// <summary>
/// Streamer.bot inline source for Twitch subs / resubs / gifted subs.
///
/// Action name: Elden Chaos Sub
/// Paste this into Core → C# → Execute C# Code, Compile.
///
/// Triggers on that same action:
///   1. Twitch → Sub
///   2. Twitch → ReSub
///   3. Twitch → Gift Subscription  (enable Ignore Gift Subs from Bombs)
///   4. Twitch → Gift Bomb
///        Check Tier 1 + Tier 2 + Tier 3, and Public + Anonymous
///
/// One sub / resub / single gift = 1 random boss.
/// A gift bomb of X = X random bosses.
/// Right-click Test Trigger on Gift Bomb uses 5 bosses if %gifts% is missing.
/// </summary>
public class CPHInline
{
    const int GameUdpPort = 22666;
    const string TtsVoiceAlias = "Zira";
    const int GiftBombTestDefault = 5;

    public bool Execute()
    {
        bool bombEvent = IsGiftBombEvent();
        if (!bombEvent && IsGiftSubFromBomb())
        {
            CPH.LogInfo("EldenChaos: skip gift-sub inside a bomb (Gift Bomb trigger handles count)");
            return true;
        }

        string user = GetUserName();
        int count = GetBossCount(bombEvent);
        if (count < 1) count = 1;

        CPH.LogInfo("EldenChaos sub count=" + count + " bombEvent=" + bombEvent +
                    " user=" + user);

        string message = count == 1
            ? user + " subscribed. A random boss appears."
            : user + " gifted " + count + " subs. " + count + " random bosses inbound.";

        string json = BuildGameJson(user, count, message);
        string overlay = BuildOverlayJson(user, count, message);

        CPH.SetArgument("chaosMessage", message);
        CPH.SetArgument("chaosKind", "boss");
        CPH.SetArgument("chaosJson", json);

        // UDP / overlay failures must not skip TTS (channel-point action often has a
        // separate Speak sub-action; this action usually does not).
        try
        {
            CPH.BroadcastUdp(GameUdpPort, json);
        }
        catch (Exception ex)
        {
            CPH.LogWarn("EldenChaos UDP failed: " + ex.Message);
        }

        SpeakTts(message);

        try
        {
            CPH.WebsocketBroadcastJson(overlay);
        }
        catch (Exception ex)
        {
            CPH.LogWarn("EldenChaos overlay failed: " + ex.Message);
        }

        CPH.LogInfo("EldenChaos: " + message);
        return true;
    }

    bool IsGiftBombEvent()
    {
        // Real bomb, or Test Trigger on the Gift Bomb row.
        if (GetIntArg("gifts") > 1) return true;
        string src = CombinedSource();
        return src.IndexOf("bomb", StringComparison.OrdinalIgnoreCase) >= 0
            || src.IndexOf("community", StringComparison.OrdinalIgnoreCase) >= 0;
    }

    bool IsGiftSubFromBomb()
    {
        try
        {
            if (CPH.TryGetArg("fromGiftBomb", out bool fromBomb) && fromBomb)
                return true;
        }
        catch { }
        try
        {
            if (CPH.TryGetArg("fromGiftBomb", out string s) &&
                (string.Equals(s, "true", StringComparison.OrdinalIgnoreCase) || s == "1"))
                return true;
        }
        catch { }
        return false;
    }

    int GetBossCount(bool bombEvent)
    {
        // Gift Bomb uses %gifts%. Never use %totalGifts% (lifetime total).
        foreach (var name in new[] { "gifts", "giftCount", "subBombCount" })
        {
            int n = GetIntArg(name);
            if (n > 1) return n;
        }
        // Right-click Test Trigger often omits %gifts%. Still spawn several bosses.
        if (bombEvent) return GiftBombTestDefault;
        return 1;
    }

    int GetIntArg(string name)
    {
        try
        {
            if (CPH.TryGetArg(name, out int n) && n > 0) return n;
        }
        catch { }
        try
        {
            if (CPH.TryGetArg(name, out long n) && n > 0 && n <= int.MaxValue) return (int)n;
        }
        catch { }
        try
        {
            if (CPH.TryGetArg(name, out double n) && n > 0) return (int)n;
        }
        catch { }
        try
        {
            if (CPH.TryGetArg(name, out string s) && !string.IsNullOrWhiteSpace(s))
            {
                if (int.TryParse(s.Trim(), out int n) && n > 0) return n;
                if (double.TryParse(s.Trim(), out double d) && d > 0) return (int)d;
            }
        }
        catch { }
        try
        {
            if (CPH.TryGetArg(name, out object o) && o != null)
            {
                if (o is int i && i > 0) return i;
                if (o is long l && l > 0 && l <= int.MaxValue) return (int)l;
                if (int.TryParse(Convert.ToString(o), out int n) && n > 0) return n;
            }
        }
        catch { }
        return 0;
    }

    string CombinedSource()
    {
        var sb = new StringBuilder();
        foreach (var name in new[] { "__source", "eventSource", "eventType", "__sourceName" })
        {
            try
            {
                if (CPH.TryGetArg(name, out string v) && !string.IsNullOrWhiteSpace(v))
                    sb.Append(' ').Append(v);
            }
            catch { }
        }
        return sb.ToString();
    }

    void SpeakTts(string message)
    {
        string text = ToTtsText(message);
        if (string.IsNullOrWhiteSpace(text)) return;

        string alias = string.IsNullOrWhiteSpace(TtsVoiceAlias) ? "" : TtsVoiceAlias;
        try
        {
            CPH.TtsSpeak(alias, text, false);
            CPH.LogInfo("EldenChaos TTS sent alias=" + alias);
            return;
        }
        catch (Exception ex)
        {
            CPH.LogWarn("EldenChaos TTS alias '" + alias + "' failed: " + ex.Message);
        }
        if (alias.Length == 0) return;
        try
        {
            CPH.TtsSpeak("", text, false);
            CPH.LogInfo("EldenChaos TTS sent via default voice");
        }
        catch (Exception ex)
        {
            CPH.LogWarn("EldenChaos TTS default failed: " + ex.Message);
        }
    }

    static string ToTtsText(string s)
    {
        if (string.IsNullOrEmpty(s)) return "";
        return s.Replace('\u2014', '-').Replace('\u2013', '-').Replace('\u00A0', ' ');
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

    static string BuildGameJson(string user, int count, string message)
    {
        var sb = new StringBuilder();
        sb.Append("{\"cmd\":\"effect\"");
        sb.Append(",\"effect\":\"boss\"");
        sb.Append(",\"target\":\"random\"");
        sb.Append(",\"enemy\":\"boss_random\"");
        sb.Append(",\"count\":").Append(count);
        sb.Append(",\"user\":\"").Append(Escape(user)).Append("\"");
        sb.Append(",\"message\":\"").Append(Escape(message)).Append("\"");
        sb.Append("}");
        return sb.ToString();
    }

    static string BuildOverlayJson(string user, int count, string message)
    {
        var sb = new StringBuilder();
        sb.Append("{\"source\":\"ockas-elden-chaos\",\"type\":\"chaos\"");
        sb.Append(",\"user\":\"").Append(Escape(user)).Append("\"");
        sb.Append(",\"eventKind\":\"boss\"");
        sb.Append(",\"target\":\"random\"");
        sb.Append(",\"message\":\"").Append(Escape(message)).Append("\"");
        sb.Append(",\"enemy\":\"boss_random\"");
        sb.Append(",\"count\":").Append(count);
        sb.Append("}");
        return sb.ToString();
    }

    static string Escape(string s)
    {
        if (string.IsNullOrEmpty(s)) return "";
        return s.Replace("\\", "\\\\").Replace("\"", "\\\"");
    }
}
