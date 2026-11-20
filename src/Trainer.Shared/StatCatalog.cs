namespace Trainer.Shared;

public static class StatIds
{
    public const string MaxHealth="maxHealth", Armor="armor", Recovery="recovery", Might="might",
        ProjectileSpeed="projectileSpeed", Duration="duration", Area="area", Cooldown="cooldown",
        Amount="amount", MoveSpeed="moveSpeed", Magnet="magnet", Luck="luck", Growth="growth",
        Greed="greed", Curse="curse", Revivals="revivals", Rerolls="rerolls", Skips="skips",
        Banishes="banishes", Charm="charm", Defang="defang", Preserve="preserve", Omni="omni", Seal="seal";
}

public enum StatValueKind { Absolute, BonusPercent, ReductionPercent, ChancePercent }

public sealed class StatDefinition
{
    public string Id { get; }
    public string Name { get; }
    public string Group { get; }
    public string Unit { get; }
    public double Min { get; }
    public double Max { get; }
    public double DefaultValue { get; }
    public int DecimalPlaces { get; }
    public StatValueKind Kind { get; }
    public string Description { get; }
    public StatDefinition(string id,string name,string group,string unit,double min,double max,double initial,int decimals,StatValueKind kind,string description)
    { Id=id;Name=name;Group=group;Unit=unit;Min=min;Max=max;DefaultValue=initial;DecimalPlaces=decimals;Kind=kind;Description=description; }
    public double ToRaw(double value) => Kind switch
    { StatValueKind.BonusPercent => 1+value/100, StatValueKind.ReductionPercent => 1-value/100, StatValueKind.ChancePercent => value/100, _ => value };
    public double FromRaw(double value) => Kind switch
    { StatValueKind.BonusPercent => (value-1)*100, StatValueKind.ReductionPercent => (1-value)*100, StatValueKind.ChancePercent => value*100, _ => value };
}

public static class StatCatalog
{
    private const StatValueKind A=StatValueKind.Absolute, B=StatValueKind.BonusPercent, R=StatValueKind.ReductionPercent, C=StatValueKind.ChancePercent;
    public static readonly IReadOnlyList<StatDefinition> All = new StatDefinition[]
    {
        new(StatIds.MaxHealth,"生命上限","生存","HP",1,1000000,500,2,A,"覆盖最大生命；不会自动填满生命。关闭后按正常上限处理。"),
        new(StatIds.Armor,"护甲","生存","点",0,10000,20,2,A,"覆盖护甲点数。"),
        new(StatIds.Recovery,"生命恢复","生存","HP/秒",0,10000,5,2,A,"覆盖每秒生命恢复。"),
        new(StatIds.Might,"力量加成","战斗","%",-100,900,100,1,B,"100代表力量加成+100%；伤害倍率在最终命中时继续相乘。"),
        new(StatIds.ProjectileSpeed,"弹射速度加成","战斗","%",-100,400,100,1,B,"覆盖角色弹速加成；武器自己的机制继续执行。"),
        new(StatIds.Duration,"持续时间加成","战斗","%",-100,400,100,1,B,"覆盖角色持续时间加成。"),
        new(StatIds.Area,"攻击范围加成","战斗","%",-100,900,100,1,B,"覆盖角色范围加成，保留武器自身范围参数。"),
        new(StatIds.Cooldown,"冷却缩减","战斗","%",0,90,90,1,R,"90代表缩短90%；快速冷却开关仍可将支持武器压至0.1秒。"),
        new(StatIds.Amount,"额外发射数量","战斗","个",0,100,10,0,A,"覆盖角色额外发射数量；实际效果受具体武器机制影响。"),
        new(StatIds.MoveSpeed,"移动速度加成","移动与拾取","%",-90,1000,200,1,B,"覆盖角色移动加成，原移动倍率在此基础上继续相乘。"),
        new(StatIds.Magnet,"拾取范围","移动与拾取","半径",0,10000,300,1,A,"覆盖实际拾取半径，与游戏面板显示范围使用同一单位。"),
        new(StatIds.Luck,"幸运加成","成长与收益","%",-100,10000,100,1,B,"覆盖幸运加成。"),
        new(StatIds.Growth,"经验成长加成","成长与收益","%",0,10000,100,1,B,"100代表经验成长+100%；原经验倍率在实际获得经验时继续相乘。"),
        new(StatIds.Greed,"金币收益加成","成长与收益","%",0,10000,100,1,B,"覆盖局内金币收益加成，不设置永久钱包。"),
        new(StatIds.Curse,"诅咒加成","敌人与刷怪","%",-100,10000,0,1,B,"覆盖本局诅咒；会影响游戏原有的敌人和生成计算。"),
        new(StatIds.Charm,"魅力","敌人与刷怪","个/波",0,1000,100,0,A,"覆盖每波额外敌人数量。"),
        new(StatIds.Defang,"去牙概率","敌人与刷怪","%",0,100,100,1,C,"使生成敌人无法造成接触伤害的概率。"),
        new(StatIds.Revivals,"复活次数","次数","次",0,1000,10,0,A,"仅覆盖当前角色本局次数，保留永久强化记录。"),
        new(StatIds.Rerolls,"重选次数","次数","次",0,10000,100,0,A,"仅覆盖本局重选次数。"),
        new(StatIds.Skips,"跳过次数","次数","次",0,10000,100,0,A,"仅覆盖本局跳过次数。"),
        new(StatIds.Banishes,"排除次数","次数","次",0,10000,100,0,A,"仅覆盖本局排除次数。"),
        new(StatIds.Preserve,"次数保留概率","次数","%",0,100,100,1,C,"重选、跳过、排除时保留次数的概率。"),
        new(StatIds.Omni,"万能统一加成","组合","%",0,400,100,1,B,"统一设定力量、弹速、持续时间、范围；已单独开启的属性优先。"),
        new(StatIds.Seal,"封印","局外收藏","项",0,10000,0,0,A,"封印属于局外收藏设置，不通过本局属性覆盖修改。")
    };
    public static StatDefinition? Find(string? id) => id is null ? null : All.FirstOrDefault(item=>item.Id==id);
    public static void Validate(StatDefinition definition,double value)
    {
        if(!double.IsFinite(value)||value<definition.Min||value>definition.Max)
            throw new ArgumentException($"{definition.Name}必须在{definition.Min}到{definition.Max}{definition.Unit}之间。");
        if(definition.DecimalPlaces==0 && value!=Math.Truncate(value))
            throw new ArgumentException($"{definition.Name}必须是整数。");
    }
}
