using System;
using System.Collections.Generic;
using System.Text;

/// <summary>
/// Streamer.bot inline source for Elden Chaos channel-point / sub rewards.
///
/// Setup (per reward):
///   1. Create a Channel Point reward.
///   2. Action → Execute C# Code (this script).
///   3. Set sub-action argument OR use reward title matching below:
///        Argument name:  chaosEffect
///        Argument value: heal_hp | restore_fp | lose_runes | give_weapon |
///                        unequip | ash_of_war | swap_hands | flasks |
///                        no_roll | invert_move | vigor | mind | endurance |
///                        strength | dexterity | intelligence | faith | arcane |
///                        lobster | crabs | bear | tree_sentinel | dogs | rats | hawks | boss
///
/// Target is chosen automatically from the catalog (all vs random).
///
/// Subs / gifted subs: use ChaosSub.cs on a separate action, not this script.
/// </summary>
public class CPHInline
{
    const int GameUdpPort = 22666;
    const string TtsVoiceAlias = "Zira";

    static readonly Random Rng = new Random();

    class EffectDef
    {
        public string Effect;   // effect= field (or special)
        public string Target;   // all | random
        public string Enemy;    // for spawn
        public int? Count;
        public string Stat;     // for stat
        public int? Delta;      // fixed delta; null => random ±5 for stats
        public int? Percent;    // runes
        public int? Ms;         // timed effects
        public string Label;    // human message fragment
    }

    static readonly Dictionary<string, EffectDef> Catalog = BuildCatalog();

    public bool Execute()
    {
        string user = GetUserName();
        string key = ResolveEffectKey();
        if (string.IsNullOrWhiteSpace(key) || !Catalog.TryGetValue(Normalize(key), out var def))
        {
            CPH.LogWarn("EldenChaos: unknown chaosEffect/reward '" + key + "'");
            return false;
        }

        // Stats listed as "−5 or +5": roll when Delta is null
        int? delta = def.Delta;
        if (def.Stat != null && delta == null)
            delta = Rng.Next(2) == 0 ? 5 : -5;

        // Flasks: one coin flip, sent to the game so every player gets the same result.
        string mode = null;
        if (def.Effect == "flasks")
        {
            mode = Rng.Next(2) == 0 ? "drain" : "refill";
            def = new EffectDef
            {
                Effect = def.Effect,
                Target = def.Target,
                Label = mode == "drain" ? "drained everyone's flasks" : "refilled everyone's flasks",
            };
        }

        string message = BuildMessage(user, def, delta);
        string json = BuildGameJson(user, def, delta, message, mode);
        string overlay = BuildOverlayJson(user, def, delta, message);

        CPH.BroadcastUdp(GameUdpPort, json);
        CPH.WebsocketBroadcastJson(overlay);

        CPH.SetArgument("chaosMessage", message);
        CPH.SetArgument("chaosKind", def.Effect);
        CPH.SetArgument("chaosJson", json);

        try
        {
            if (!string.IsNullOrWhiteSpace(TtsVoiceAlias))
                CPH.TtsSpeak(TtsVoiceAlias, message, false);
        }
        catch (Exception ex)
        {
            CPH.LogWarn("EldenChaos TTS skipped: " + ex.Message);
        }

        CPH.LogInfo("EldenChaos: " + message);
        return true;
    }

    string ResolveEffectKey()
    {
        // 1) Explicit argument on the action
        try
        {
            if (CPH.TryGetArg("chaosEffect", out string effect) && !string.IsNullOrWhiteSpace(effect))
                return effect;
        }
        catch { }

        // 2) Reward title / name from Twitch redemption
        foreach (var arg in new[] { "rewardName", "rewardTitle", "reward", "__source" })
        {
            try
            {
                if (CPH.TryGetArg(arg, out string v) && !string.IsNullOrWhiteSpace(v))
                {
                    string mapped = MapRewardTitle(v);
                    if (mapped != null) return mapped;
                }
            }
            catch { }
        }
        return null;
    }

    static string MapRewardTitle(string title)
    {
        string t = Normalize(title);
        // Exact catalog keys
        if (Catalog.ContainsKey(t)) return t;

        // Friendly Twitch reward titles
        if (t.Contains("heal")) return "heal_hp";
        if (t.Contains("full fp") || t.Contains("restore fp") || t == "fp") return "restore_fp";
        if (t.Contains("rune")) return "lose_runes";
        if (t.Contains("give") && t.Contains("weapon")) return "give_weapon";
        if (t.Contains("unequip")) return "unequip";
        if (t.Contains("ash")) return "ash_of_war";
        if (t.Contains("swap") || (t.Contains("left") && t.Contains("right"))) return "swap_hands";
        if (t.Contains("flask")) return "flasks";
        if (t.Contains("roll")) return "no_roll";
        if (t.Contains("invert")) return "invert_move";
        if (t.Contains("lobster")) return "lobster";
        if (t.Contains("crab")) return "crabs";
        if (t.Contains("bear")) return "bear";
        if (t.Contains("tree sentinel") || t.Contains("treesentinel")) return "tree_sentinel";
        if (t.Contains("dog")) return "dogs";
        if (t.Contains("rat")) return "rats";
        if (t.Contains("hawk") || t.Contains("warhawk")) return "hawks";
        if (t.Contains("boss") || t.Contains("sub")) return "boss";

        foreach (var stat in new[] { "vigor", "mind", "endurance", "strength", "dexterity", "intelligence", "faith", "arcane" })
        {
            if (t.Contains(stat)) return stat;
        }
        return null;
    }

    static string BuildMessage(string user, EffectDef def, int? delta)
    {
        string who = def.Target == "random" ? "a random player" : "everyone";
        if (def.Stat != null)
        {
            int d = delta ?? 0;
            string sign = d > 0 ? ("+" + d) : d.ToString();
            string pretty = char.ToUpper(def.Stat[0]) + def.Stat.Substring(1);
            return user + " gave " + sign + " " + pretty + " to " + who;
        }
        if (def.Effect == "spawn")
        {
            int n = def.Count ?? 1;
            return user + " spawned " + n + " " + def.Label + " on " + who;
        }
        if (def.Effect == "boss")
        {
            int n = def.Count ?? 1;
            if (n <= 1)
                return user + " summoned a random main boss on " + who;
            return user + " summoned " + n + " random bosses on " + who;
        }
        return user + " " + def.Label;
    }

    static string BuildGameJson(string user, EffectDef def, int? delta, string message, string mode)
    {
        var sb = new StringBuilder();
        sb.Append("{\"cmd\":\"effect\"");
        sb.Append(",\"effect\":\"").Append(Escape(def.Effect)).Append("\"");
        sb.Append(",\"target\":\"").Append(Escape(def.Target)).Append("\"");
        if (def.Enemy != null) sb.Append(",\"enemy\":\"").Append(Escape(def.Enemy)).Append("\"");
        if (def.Count.HasValue) sb.Append(",\"count\":").Append(def.Count.Value);
        if (def.Stat != null) sb.Append(",\"stat\":\"").Append(Escape(def.Stat)).Append("\"");
        if (delta.HasValue) sb.Append(",\"delta\":").Append(delta.Value);
        if (def.Percent.HasValue) sb.Append(",\"percent\":").Append(def.Percent.Value);
        if (def.Ms.HasValue) sb.Append(",\"ms\":").Append(def.Ms.Value);
        if (mode != null) sb.Append(",\"mode\":\"").Append(mode).Append("\"");
        if (def.Effect == "boss" && def.Enemy == null)
        {
            sb.Append(",\"enemy\":\"boss_random\"");
        }
        if (def.Effect == "boss" && !def.Count.HasValue)
        {
            sb.Append(",\"count\":1");
        }
        sb.Append(",\"user\":\"").Append(Escape(user)).Append("\"");
        sb.Append(",\"message\":\"").Append(Escape(message)).Append("\"");
        sb.Append("}");
        return sb.ToString();
    }

    static string BuildOverlayJson(string user, EffectDef def, int? delta, string message)
    {
        var sb = new StringBuilder();
        sb.Append("{\"source\":\"ockas-elden-chaos\",\"type\":\"chaos\"");
        sb.Append(",\"user\":\"").Append(Escape(user)).Append("\"");
        sb.Append(",\"eventKind\":\"").Append(Escape(def.Effect)).Append("\"");
        sb.Append(",\"target\":\"").Append(Escape(def.Target)).Append("\"");
        sb.Append(",\"message\":\"").Append(Escape(message)).Append("\"");
        if (def.Enemy != null) sb.Append(",\"enemy\":\"").Append(Escape(def.Enemy)).Append("\"");
        if (def.Count.HasValue) sb.Append(",\"count\":").Append(def.Count.Value);
        if (def.Stat != null) sb.Append(",\"stat\":\"").Append(Escape(def.Stat)).Append("\"");
        if (delta.HasValue) sb.Append(",\"delta\":").Append(delta.Value);
        sb.Append("}");
        return sb.ToString();
    }

    static Dictionary<string, EffectDef> BuildCatalog()
    {
        var d = new Dictionary<string, EffectDef>(StringComparer.OrdinalIgnoreCase);

        void Add(string key, EffectDef def) => d[Normalize(key)] = def;

        // ---- ALL (channel points) ----
        Add("heal_hp", new EffectDef { Effect = "heal_hp", Target = "all", Label = "healed everyone's HP" });
        Add("restore_fp", new EffectDef { Effect = "restore_fp", Target = "all", Label = "restored everyone's FP" });
        Add("lose_runes", new EffectDef { Effect = "lose_runes", Target = "all", Percent = 10, Label = "took 10% of everyone's runes" });
        Add("give_weapon", new EffectDef { Effect = "give_weapon", Target = "all", Label = "gave everyone a random weapon" });
        Add("unequip", new EffectDef { Effect = "unequip", Target = "all", Label = "unequipped random gear on everyone" });
        Add("ash_of_war", new EffectDef { Effect = "ash_of_war", Target = "all", Label = "randomized everyone's Ash of War" });
        Add("swap_hands", new EffectDef { Effect = "swap_hands", Target = "all", Label = "swapped everyone's left and right hand" });
        Add("flasks", new EffectDef { Effect = "flasks", Target = "all", Label = "rolled everyone's flasks" });
        Add("no_roll", new EffectDef { Effect = "no_roll", Target = "all", Ms = 5000, Label = "disabled rolling for everyone for 5 seconds" });
        Add("invert_move", new EffectDef { Effect = "invert_move", Target = "all", Ms = 10000, Label = "inverted everyone's movement for 10 seconds" });

        foreach (var stat in new[] { "vigor", "mind", "endurance", "strength", "dexterity", "intelligence", "faith", "arcane" })
        {
            Add(stat, new EffectDef { Effect = "stat", Target = "all", Stat = stat, Delta = null, Label = stat });
        }

        // ---- RANDOM (channel points) ----
        Add("lobster", new EffectDef { Effect = "spawn", Target = "random", Enemy = "lobster", Count = 1, Label = "lobster" });
        Add("crabs", new EffectDef { Effect = "spawn", Target = "random", Enemy = "crabs", Count = 3, Label = "crabs" });
        Add("bear", new EffectDef { Effect = "spawn", Target = "random", Enemy = "bear", Count = 1, Label = "bear" });
        Add("tree_sentinel", new EffectDef { Effect = "spawn", Target = "random", Enemy = "tree_sentinel", Count = 1, Label = "Tree Sentinel" });
        Add("dogs", new EffectDef { Effect = "spawn", Target = "random", Enemy = "dogs", Count = 3, Label = "dogs" });
        Add("rats", new EffectDef { Effect = "spawn", Target = "random", Enemy = "rats", Count = 3, Label = "rats" });
        Add("hawks", new EffectDef { Effect = "spawn", Target = "random", Enemy = "hawks", Count = 3, Label = "warhawks" });

        // ---- SUB ----
        Add("boss", new EffectDef { Effect = "boss", Target = "random", Enemy = "boss_random", Count = 1, Label = "random main boss" });

        return d;
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

    static string Normalize(string s)
    {
        if (string.IsNullOrEmpty(s)) return "";
        var sb = new StringBuilder();
        foreach (char ch in s.Trim().ToLowerInvariant())
        {
            if (char.IsLetterOrDigit(ch)) sb.Append(ch);
            else if (ch == ' ' || ch == '-' || ch == '_') sb.Append('_');
        }
        return sb.ToString().Trim('_');
    }

    static string Escape(string s)
    {
        if (string.IsNullOrEmpty(s)) return "";
        return s.Replace("\\", "\\\\").Replace("\"", "\\\"");
    }
}
