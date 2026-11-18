"""电台地区映射表

Radio-Browser 的中国台 `state` 字段用的是威妥玛 / 邮政式拼音（Kiangsu / Shantung
/ Hopei ...），必须归一化成稳定的 key + 中文名，供 /api/radio/regions 和目录清洗使用。
"""
import re

# ── 行政区划标准顺序（GB/T 2260）：(key, 中文名, 顺序号, 上游 state 别名) ─────
PROVINCES = [
    ("beijing",       "北京",   1,  ["beijing", "peking"]),
    ("tianjin",       "天津",   2,  ["tianjin", "tientsin"]),
    ("hebei",         "河北",   3,  ["hebei", "hopei"]),
    ("shanxi",        "山西",   4,  ["shanxi", "shansi"]),
    ("neimenggu",     "内蒙古", 5,  ["inner mongolia", "nei mongol"]),
    ("liaoning",      "辽宁",   6,  ["liaoning", "fengtien"]),
    ("jilin",         "吉林",   7,  ["jilin", "kirin"]),
    ("heilongjiang",  "黑龙江", 8,  ["heilongjiang", "heilungkiang"]),
    ("shanghai",      "上海",   9,  ["shanghai"]),
    ("jiangsu",       "江苏",  10,  ["jiangsu", "kiangsu"]),
    ("zhejiang",      "浙江",  11,  ["zhejiang", "chekiang"]),
    ("anhui",         "安徽",  12,  ["anhui", "anhwei"]),
    ("fujian",        "福建",  13,  ["fujian", "fukien"]),
    ("jiangxi",       "江西",  14,  ["jiangxi", "kiangsi"]),
    ("shandong",      "山东",  15,  ["shandong", "shantung"]),
    ("henan",         "河南",  16,  ["henan", "honan"]),
    ("hubei",         "湖北",  17,  ["hubei", "hupeh"]),
    ("hunan",         "湖南",  18,  ["hunan"]),
    ("guangdong",     "广东",  19,  ["guangdong", "kwangtung"]),
    ("guangxi",       "广西",  20,  ["guangxi", "kwangsi"]),
    ("hainan",        "海南",  21,  ["hainan"]),
    ("chongqing",     "重庆",  22,  ["chongqing", "chungking"]),
    ("sichuan",       "四川",  23,  ["sichuan", "szechuan", "szechwan"]),
    ("guizhou",       "贵州",  24,  ["guizhou", "kweichow"]),
    ("yunnan",        "云南",  25,  ["yunnan"]),
    ("xizang",        "西藏",  26,  ["tibet", "xizang"]),
    ("shaanxi",       "陕西",  27,  ["shaanxi", "shensi"]),
    ("gansu",         "甘肃",  28,  ["gansu", "kansu"]),
    ("qinghai",       "青海",  29,  ["qinghai", "tsinghai"]),
    ("ningxia",       "宁夏",  30,  ["ningxia", "ninghsia"]),
    ("xinjiang",      "新疆",  31,  ["xinjiang", "sinkiang"]),
    ("taiwan",        "台湾",  32,  ["taiwan", "formosa"]),
    ("xianggang",     "香港",  33,  ["hong kong", "xianggang", "hk"]),
    ("aomen",         "澳门",  34,  ["macau", "macao", "aomen"]),
]

REGION_NAME = {key: name for key, name, _order, _alias in PROVINCES}
REGION_ORDER = {key: order for key, _name, order, _alias in PROVINCES}

# 别名（小写）→ key
_ALIAS_TO_KEY: dict[str, str] = {}
for key, _name, _order, aliases in PROVINCES:
    for a in aliases:
        _ALIAS_TO_KEY[a.strip().lower()] = key

# ── 城市 → 省 兜底（上游 state 里混有城市名时）───────────────────────────────
# 罗马字城市名 → 省 key
_CITY_ROMAN_TO_PROVINCE = {
    "dalian": "liaoning", "shenyang": "liaoning",
    "shenzhen": "guangdong", "guangzhou": "guangdong", "dongguan": "guangdong",
    "xiamen": "fujian", "fuzhou": "fujian",
    "chengdu": "sichuan",
    "hangzhou": "zhejiang", "ningbo": "zhejiang", "wenzhou": "zhejiang",
    "nanjing": "jiangsu", "suzhou": "jiangsu", "wuxi": "jiangsu",
    "wuhan": "hubei",
    "xian": "shaanxi", "xi'an": "shaanxi",
    "harbin": "heilongjiang",
    "qingdao": "shandong", "jinan": "shandong",
    "zhengzhou": "henan",
    "changsha": "hunan",
    "kunming": "yunnan",
    "nanning": "guangxi",
    "haikou": "hainan", "sanya": "hainan",
    "taipei": "taiwan", "kaohsiung": "taiwan",
    "lanzhou": "gansu", "guiyang": "guizhou", "shijiazhuang": "hebei",
    "taiyuan": "shanxi", "changchun": "jilin", "hefei": "anhui",
    "nanchang": "jiangxi", "hohhot": "neimenggu", "urumqi": "xinjiang",
    "lhasa": "xizang", "yinchuan": "ningxia", "xining": "qinghai",
    "taizhou": "zhejiang",
}

# 中文城市名 → 省 key（解析台名兜底）
_CITY_CHN_TO_PROVINCE = {
    "大连": "liaoning", "沈阳": "liaoning", "营口": "liaoning",
    "深圳": "guangdong", "广州": "guangdong", "东莞": "guangdong", "佛山": "guangdong",
    "怀集": "guangdong", "顺德": "guangdong",
    "厦门": "fujian", "福州": "fujian", "泉州": "fujian",
    "成都": "sichuan", "彭州": "sichuan",
    "杭州": "zhejiang", "宁波": "zhejiang", "温州": "zhejiang",
    "南京": "jiangsu", "苏州": "jiangsu", "无锡": "jiangsu", "常州": "jiangsu",
    "武汉": "hubei",
    "西安": "shaanxi",
    "哈尔滨": "heilongjiang",
    "青岛": "shandong", "济南": "shandong", "烟台": "shandong",
    "郑州": "henan", "洛阳": "henan",
    "长沙": "hunan",
    "昆明": "yunnan",
    "南宁": "guangxi", "桂林": "guangxi",
    "海口": "hainan", "三亚": "hainan",
    "台北": "taiwan", "高雄": "taiwan",
    "兰州": "gansu", "贵阳": "guizhou", "石家庄": "hebei", "唐山": "hebei",
    "太原": "shanxi", "长春": "jilin", "合肥": "anhui", "芜湖": "anhui",
    "南昌": "jiangxi", "呼和浩特": "neimenggu", "乌鲁木齐": "xinjiang",
    "拉萨": "xizang", "银川": "ningxia", "西宁": "qinghai", "台州": "zhejiang",
}

# 中文省名/直辖市/自治区名 → key（台名兜底用）
_NAME_TO_KEY = {name: key for key, name, _order, _alias in PROVINCES}

# 台名里可能出现的中文地名（省名 + 城市名），按长度降序，避免短词误截长词
_CHN_KEYWORDS = sorted(
    [(_NAME_TO_KEY[n], n) for n in _NAME_TO_KEY]
    + [(_CITY_CHN_TO_PROVINCE[c], c) for c in _CITY_CHN_TO_PROVINCE],
    key=lambda kv: len(kv[1]),
    reverse=True,
)

# ── 国家级电台关键词（台名/标签命中 → nationwide）────────────────────────────
# 强信号：台名/标签命中即判为国家级（但台名含明确地名时，地名优先于本表）
_NATIONWIDE_STRONG = (
    "中国之声", "中国交通广播", "中国乡村之声",
    "cnr", "央广", "中央人民广播", "中国国际广播", "cri", "cgtn",
    "hit fm", "轻松调频",
)

# 弱信号：频道名本身，但本地台常带地名前缀（如"广东音乐之声"），须在地名匹配之后判
_NATIONWIDE_WEAK = (
    "经济之声", "音乐之声", "都市之声", "文艺之声", "老年之声",
    "神州之声", "民族之声", "阅读之声", "台海之声", "乡村之声",
    "环球资讯", "南海之声", "海峡飞虹",
)


def classify_cn(state: str, name: str, tags: str) -> tuple[str, str]:
    """把一条中国台归到 (region_key, region_name)。

    判定顺序：
      1. 台名里的中文地名 → 省（台名明确带地名即地方台，优先于国家级关键词；
         "广东音乐之声"命中"广东"，"青岛FM96.4 CGTN"命中"青岛"）
      2. 强国家级信号（央广/CRI/“中国XX”频道）→ 全国
      3. 弱国家级频道名（"音乐之声"等裸频道名）→ 全国
      4. state 字段 → 省/城市
      5. other
    """
    hay = f"{name or ''} {tags or ''}".lower()

    # 1) 台名中文地名（省名/主要城市）——地方台信号最强，优先于国家级关键词
    if name:
        for key, chn in _CHN_KEYWORDS:
            if chn in name:
                return key, REGION_NAME[key]

    # 2) 强国家级信号
    for kw in _NATIONWIDE_STRONG:
        if kw in hay:
            return "nationwide", "全国"

    # 3) 弱国家级频道名（此时台名不含地名，裸"音乐之声"归全国）
    for kw in _NATIONWIDE_WEAK:
        if kw in hay:
            return "nationwide", "全国"

    # 4) state 字段：省/自治区别名 → 城市
    s = (state or "").strip().lower()
    if s:
        key = _ALIAS_TO_KEY.get(s)
        if key is None:
            key = _CITY_ROMAN_TO_PROVINCE.get(s)
        if key:
            return key, REGION_NAME[key]

    return "other", "其他"


def slugify(text: str) -> str:
    """把任意字符串转成稳定的 URL key（美国州名用）。"""
    out: list[str] = []
    prev_dash = False
    for ch in (text or "").strip().lower():
        if ch.isalnum():
            out.append(ch)
            prev_dash = False
        elif not prev_dash:
            out.append("-")
            prev_dash = True
    return "".join(out).strip("-")


def classify_us(state: str) -> tuple[str, str]:
    """把一条美国台归到 (region_key, region_name)。

    上游 US `state` 字段常混有 "City ST" / "City, State" 形式（如 "Tucson AZ"、
    "Los Angeles, California"），这里从整串里提取州全名或 2 字母缩写做归一化。
    """
    s = (state or "").strip()
    if not s:
        return "other", "其他"
    low = s.lower()

    # 0) 明显非州（URL / 国家名 / 通用词）→ other
    if low.startswith("http") or low in _US_NON_STATE:
        return "other", "其他"
    # 0.5) 常见城市名 → 州
    key = _US_CITY_TO_STATE.get(low)
    if key:
        return key, _US_DISPLAY[key]

    # 1) 整串就是州全名/别名（含 "washington dc" 之类）
    key = _US_NAME.get(low)
    if key:
        return key, _US_DISPLAY[key]

    # 2) 整串含州全名（如 "Los Angeles, California"），长名优先避免 "new york" 被截断
    for name, key in _US_NAMES_SORTED:
        if name in low:
            return key, _US_DISPLAY[key]

    # 3) 含独立 2 字母州缩写（如 "Tucson AZ"），按词切分避免 "richmond" 里的 "mo" 误配
    for tok in re.findall(r"[a-z]+", low):
        if len(tok) == 2 and tok in _US_ABBR:
            return _US_ABBR[tok], _US_DISPLAY[_US_ABBR[tok]]

    # 兜底：整串当州名
    return slugify(s), s


# ── 美国 50 州 + 华盛顿特区 ────────────────────────────────────
# (key, 规范名, [缩写 + 别名（小写）])
US_STATES = [
    ("alabama",              "Alabama",              ["al"]),
    ("alaska",               "Alaska",               ["ak"]),
    ("arizona",              "Arizona",              ["az"]),
    ("arkansas",             "Arkansas",             ["ar"]),
    ("california",           "California",           ["ca"]),
    ("colorado",             "Colorado",             ["co"]),
    ("connecticut",          "Connecticut",          ["ct"]),
    ("delaware",             "Delaware",             ["de"]),
    ("florida",              "Florida",              ["fl"]),
    ("georgia",              "Georgia",              ["ga"]),
    ("hawaii",               "Hawaii",               ["hi"]),
    ("idaho",                "Idaho",                ["id"]),
    ("illinois",             "Illinois",             ["il"]),
    ("indiana",              "Indiana",              ["in"]),
    ("iowa",                 "Iowa",                 ["ia"]),
    ("kansas",               "Kansas",               ["ks"]),
    ("kentucky",             "Kentucky",             ["ky"]),
    ("louisiana",            "Louisiana",            ["la"]),
    ("maine",                "Maine",                ["me"]),
    ("maryland",             "Maryland",             ["md"]),
    ("massachusetts",        "Massachusetts",        ["ma"]),
    ("michigan",             "Michigan",             ["mi"]),
    ("minnesota",            "Minnesota",            ["mn"]),
    ("mississippi",          "Mississippi",          ["ms"]),
    ("missouri",             "Missouri",             ["mo"]),
    ("montana",              "Montana",              ["mt"]),
    ("nebraska",             "Nebraska",             ["ne"]),
    ("nevada",               "Nevada",               ["nv"]),
    ("new-hampshire",        "New Hampshire",        ["nh"]),
    ("new-jersey",           "New Jersey",           ["nj"]),
    ("new-mexico",           "New Mexico",           ["nm"]),
    ("new-york",             "New York",             ["ny"]),
    ("north-carolina",       "North Carolina",       ["nc"]),
    ("north-dakota",         "North Dakota",         ["nd"]),
    ("ohio",                 "Ohio",                 ["oh"]),
    ("oklahoma",             "Oklahoma",             ["ok"]),
    ("oregon",               "Oregon",               ["or"]),
    ("pennsylvania",         "Pennsylvania",         ["pa"]),
    ("rhode-island",         "Rhode Island",         ["ri"]),
    ("south-carolina",       "South Carolina",       ["sc"]),
    ("south-dakota",         "South Dakota",         ["sd"]),
    ("tennessee",            "Tennessee",            ["tn"]),
    ("texas",                "Texas",                ["tx"]),
    ("utah",                 "Utah",                 ["ut"]),
    ("vermont",              "Vermont",              ["vt"]),
    ("virginia",             "Virginia",             ["va"]),
    ("washington",           "Washington",           ["wa"]),
    ("west-virginia",        "West Virginia",        ["wv"]),
    ("wisconsin",            "Wisconsin",            ["wi"]),
    ("wyoming",              "Wyoming",              ["wy"]),
    ("district-of-columbia", "District of Columbia", ["dc", "washington dc"]),
]

_US_NAME: dict[str, str] = {}
_US_ABBR: dict[str, str] = {}
_US_DISPLAY: dict[str, str] = {}
for key, name, abbrs in US_STATES:
    _US_DISPLAY[key] = name
    _US_NAME[name.lower()] = key
    for a in abbrs:
        _US_NAME[a.lower()] = key
        _US_ABBR[a.lower()] = key

# 全名（不含 2 字母缩写）按长度降序，供 step 2 长名优先匹配。
# 缩写不能进 step 2 的子串匹配，否则 "Springfield" 里的 "in" 会被误判成 Indiana。
_US_NAMES_SORTED = sorted(
    ((name.lower(), key) for key, name, _abbrs in US_STATES),
    key=lambda kv: -len(kv[0]),
)

# 常见城市名 → 州 key（上游 state 字段偶尔只有城市名）
_US_CITY_TO_STATE = {
    "las vegas": "nevada", "los angeles": "california", "san francisco": "california",
    "san diego": "california", "san jose": "california", "sacramento": "california",
    "new york city": "new-york", "chicago": "illinois", "houston": "texas",
    "dallas": "texas", "austin": "texas", "san antonio": "texas",
    "phoenix": "arizona", "tucson": "arizona", "philadelphia": "pennsylvania",
    "seattle": "washington", "denver": "colorado", "boston": "massachusetts",
    "nashville": "tennessee", "atlanta": "georgia", "miami": "florida",
    "orlando": "florida", "detroit": "michigan", "portland": "oregon",
    "minneapolis": "minnesota", "pittsburgh": "pennsylvania", "cleveland": "ohio",
    "baltimore": "maryland", "st louis": "missouri", "kansas city": "missouri",
    "omaha": "nebraska", "oklahoma city": "oklahoma", "albuquerque": "new-mexico",
    "honolulu": "hawaii", "new orleans": "louisiana", "indianapolis": "indiana",
    "charlotte": "north-carolina", "memphis": "tennessee", "buffalo": "new-york",
    "salt lake city": "utah",
}

# 明显非州的国家名/通用词（→ other）
_US_NON_STATE = {
    "united states", "united states of america", "usa", "us", "u.s.", "u.s.a.",
    "puerto rico", "canada", "mexico", "online", "internet", "web", "worldwide",
}
