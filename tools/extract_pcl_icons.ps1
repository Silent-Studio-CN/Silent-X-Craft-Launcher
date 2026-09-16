# 从 PCL 源码的 XAML 里抽取矢量图标(Logo="..."/Data="..."),输出为独立 SVG。
# 用法: pwsh -File tools/extract_pcl_icons.ps1 [-PclRoot <PCL 源码根>] [-OutDir <输出目录>]
#
# 为什么要这么干:PCL 的界面图标不是图片文件,而是内嵌在 XAML 属性里的 SVG 路径数据
# (例如 PageDownloadLeft.xaml 里 Title="Mod" Logo="M789.504 912.896h-195.072a35.328...").
# 抽成独立 SVG 之后,我们的 Qt 界面可以直接用 QSvgRenderer 渲染并按主题着色。
#
# 两种图标都要处理(踩过):
#   - 填充式:元素只有 Logo/Data  -> <path fill="currentColor"/>
#   - 描边式:元素还带 StrokeThickness(如 FormMain.xaml 的 ShapeTitleLogo) -> 必须用 stroke,
#     否则开放的细线路径填充后什么都看不见(自测覆盖率 < 0.5% 才发现)
param(
    [string]$PclRoot = 'C:\dsh_work\dev\PCL',
    [string]$OutDir = (Join-Path $PSScriptRoot '..\assets\icons\pcl')
)

$ErrorActionPreference = 'Stop'
if (-not (Test-Path $PclRoot)) { throw "找不到 PCL 源码: $PclRoot" }
$OutDir = [System.IO.Path]::GetFullPath($OutDir)
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

$knownNames = @{
    '原版游戏' = 'vanilla'; 'Mod' = 'mod'; '模组' = 'mod'; '整合包' = 'modpack'
    '数据包' = 'datapack'; '资源包' = 'resourcepack'; '光影' = 'shader'; '光影包' = 'shader'
    '地图' = 'map'; '世界' = 'world'; '存档' = 'save'; '刷新' = 'refresh'
    '设置' = 'settings'; '搜索' = 'search'; '下载' = 'download'; '安装' = 'install'
    '启动' = 'launch'; '删除' = 'delete'; '重命名' = 'rename'; '文件夹' = 'folder'
    '导入' = 'import'; '导出' = 'export'; '帮助' = 'help'; '关于' = 'about'
}

function Get-Slug([string]$name, [string]$path) {
    if ($name -and $knownNames.ContainsKey($name)) { return $knownNames[$name] }
    $ascii = ($name -replace '[^A-Za-z0-9]+', '')
    if ($ascii.Length -ge 3 -and $ascii.Length -le 24) { return $ascii.ToLower() }
    $sha = [System.Security.Cryptography.SHA1]::Create()
    $h = $sha.ComputeHash([System.Text.Encoding]::UTF8.GetBytes($path))
    return 'icon_' + ([System.BitConverter]::ToString($h).Replace('-', '').Substring(0, 8).ToLower())
}

$seen = @{}        # 路径前缀 -> 文件名(内容去重)
$used = @{}        # 文件名 -> $true(避免重名互相覆盖)
$index = New-Object System.Collections.Generic.List[string]
$index.Add("# 从 PCL 源码抽取的矢量图标索引(File<TAB>名称<TAB>来源文件<TAB>样式<TAB>路径长度)")

$files = Get-ChildItem $PclRoot -Recurse -File -Include '*.xaml' -ErrorAction SilentlyContinue
foreach ($f in $files) {
    $text = Get-Content $f.FullName -Raw
    foreach ($m in [regex]::Matches($text, '<(?<tag>[A-Za-z:][\w:.]*)\b(?<attrs>[^>]*?)\b(?:Logo|Data)="(?<d>M[^"]{20,})"(?<rest>[^>]*)>')) {
        $attrs = $m.Groups['attrs'].Value + ' ' + $m.Groups['rest'].Value
        $data = $m.Groups['d'].Value
        $key = $data.Substring(0, [Math]::Min(64, $data.Length))
        if ($seen.ContainsKey($key)) { continue }

        $name = ''
        foreach ($attrName in 'Title', 'x:Name', 'Name', 'ToolTip') {
            $am = [regex]::Match($attrs, $attrName + '="(?<v>[^"]{1,40})"')
            if ($am.Success) { $name = $am.Groups['v'].Value; break }
        }
        $slug = Get-Slug $name $data
        $base = $slug; $n = 2
        while ($used.ContainsKey($base)) { $base = $slug + '_' + $n; $n++ }
        $used[$base] = $true
        $seen[$key] = $base + '.svg'

        # 规则(踩过两次才对):WPF 里 Fill 默认是黑色、Stroke 默认是 null,
        # 所以"只有 Data 没有 Stroke"的路径是**填充**图标(PathPickaxe 就是这种);
        # 只有显式写了 Stroke= 且没有 Fill 的才是描边图标(FormMain 的 ShapeTitleLogo)。
        $hasFill = [regex]::IsMatch($attrs, 'Fill="[^"]+"') -and
                   -not [regex]::IsMatch($attrs, 'Fill="(Transparent|\{x:Null\})"')
        $hasStroke = [regex]::IsMatch($attrs, 'Stroke="[^"]+"')
        $stroke = [regex]::Match($attrs, 'StrokeThickness="(?<v>[0-9.]+)"')
        $style = 'fill'
        $pathTag = '<path fill="currentColor" d="' + $data + '"/>'
        if ($hasStroke -and -not $hasFill) {
            $style = 'stroke'
            $pathTag = '<path fill="none" stroke="currentColor" stroke-width="' + $stroke.Groups['v'].Value +
                       '" stroke-linecap="round" stroke-linejoin="round" d="' + $data + '"/>'
        }

        $svg = '<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 1024 1024" width="1024" height="1024">' + [char]10 +
               '  ' + $pathTag + [char]10 + '</svg>' + [char]10
        Set-Content -Path (Join-Path $OutDir ($base + '.svg')) -Value $svg -Encoding utf8NoBOM
        $src = $f.FullName.Replace($PclRoot, '').TrimStart('\')
        $index.Add(($base + '.svg' + [char]9 + ($(if ($name) { $name } else { '(无名)' })) + [char]9 + $src + [char]9 + $style + [char]9 + $data.Length))
    }
}

Set-Content -Path (Join-Path $OutDir 'index.tsv') -Value ($index -join [char]10) -Encoding utf8NoBOM
Write-Output ("抽出图标 " + $seen.Count + " 个(其中描边式 " + (($index | Select-String -Pattern ([char]9 + 'stroke' + [char]9) | Measure-Object).Count) + " 个) -> " + $OutDir)
