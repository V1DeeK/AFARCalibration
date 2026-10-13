<#
.SYNOPSIS
    Перенос сборки агентов из проекта в учётную запись Cursor или в плагин.

.DESCRIPTION
    Проектная конфигурация лежит в .cursor/ и действует только в этом
    репозитории. Скрипт переносит её туда, где она работает во всех проектах:

      Copy      — скопировать в %USERPROFILE%\.cursor (независимая копия)
      Link      — подключить соединениями каталогов (правки видны сразу тут и там)
      Plugin    — собрать плагин Cursor в dist\ для раздачи на другие машины
      Status    — показать, что уже установлено
      Uninstall — убрать установленное из %USERPROFILE%\.cursor

    По умолчанию переносится только переносимая часть. Файлы, привязанные к
    этому проекту (команды сборки Qt, контракты SCPI и PS, стиль C++), остаются в
    репозитории — глобально они давали бы неверные указания. Ключ
    -IncludeProjectSpecific переносит и их.

.EXAMPLE
    .\.cursor\tools\install-global.ps1 -Mode Status
    .\.cursor\tools\install-global.ps1 -Mode Copy
    .\.cursor\tools\install-global.ps1 -Mode Link -Force
    .\.cursor\tools\install-global.ps1 -Mode Plugin
#>
[CmdletBinding()]
param(
    [ValidateSet('Copy', 'Link', 'Plugin', 'Status', 'Uninstall')]
    [string]$Mode = 'Status',

    [switch]$IncludeProjectSpecific,
    [switch]$Force
)

$ErrorActionPreference = 'Stop'
# Windows PowerShell 5.1 иначе выводит русский текст в кодировке консоли.
try { [Console]::OutputEncoding = [System.Text.Encoding]::UTF8 } catch { }

$SourceRoot = Split-Path -Parent (Split-Path -Parent $PSCommandPath)   # ...\.cursor
$ProjectRoot = Split-Path -Parent $SourceRoot
$UserRoot = Join-Path $env:USERPROFILE '.cursor'

# Привязано к этому репозиторию: команды сборки, контракт протокола, стиль C++.
$ProjectSpecific = @(
    'rules/project-context.mdc',
    'rules/cpp-qt-style.mdc',
    'rules/testing.mdc',
    'skills/build-and-test'
)

$Groups = @(
    @{ Name = 'agents'; Pattern = '*.md' },
    @{ Name = 'skills'; Pattern = '*' },
    @{ Name = 'rules';  Pattern = '*.mdc' }
)

function Test-ProjectSpecific([string]$RelativePath) {
    $normalized = $RelativePath -replace '\\', '/'
    foreach ($item in $ProjectSpecific) {
        if ($normalized -eq $item -or $normalized -like "$item/*") { return $true }
    }
    return $false
}

function Get-Items([string]$Group, [string]$Pattern) {
    $dir = Join-Path $SourceRoot $Group
    if (-not (Test-Path $dir)) { return @() }

    Get-ChildItem -LiteralPath $dir -Filter $Pattern | ForEach-Object {
        $relative = "$Group/$($_.Name)"
        [pscustomobject]@{
            Name            = $_.Name
            Group           = $Group
            Source          = $_.FullName
            Relative        = $relative
            ProjectSpecific = (Test-ProjectSpecific $relative)
        }
    }
}

function Get-Selection {
    $all = foreach ($g in $Groups) { Get-Items $g.Name $g.Pattern }
    if ($IncludeProjectSpecific) { return $all }
    $all | Where-Object { -not $_.ProjectSpecific }
}

function Show-Status {
    Write-Host "Источник: $SourceRoot"
    Write-Host "Учётная запись: $UserRoot`n"

    foreach ($g in $Groups) {
        Write-Host "[$($g.Name)]"
        foreach ($item in (Get-Items $g.Name $g.Pattern)) {
            $target = Join-Path (Join-Path $UserRoot $g.Name) $item.Name
            $state = if (-not (Test-Path $target)) { 'не установлено' }
                     elseif ((Get-Item $target).LinkType) { 'соединение' }
                     else { 'копия' }
            $tag = if ($item.ProjectSpecific) { ' (только для этого проекта)' } else { '' }
            Write-Host ("  {0,-24} {1}{2}" -f $item.Name, $state, $tag)
        }
        Write-Host ''
    }

    Write-Host 'Правила уровня учётной записи в %USERPROFILE%\.cursor\rules работают'
    Write-Host 'на этой машине, но между устройствами Cursor их не синхронизирует.'
    Write-Host 'Для нескольких машин используйте -Mode Plugin или git-репозиторий.'
}

function Install-Items([string]$InstallMode) {
    $selection = Get-Selection
    if (-not $selection) { throw 'Нечего переносить: не найдено ни одного файла.' }

    foreach ($item in $selection) {
        $groupDir = Join-Path $UserRoot $item.Group
        New-Item -ItemType Directory -Path $groupDir -Force | Out-Null
        $target = Join-Path $groupDir $item.Name

        if (Test-Path $target) {
            if (-not $Force) {
                Write-Host "  пропущено (уже есть): $($item.Relative)" -ForegroundColor DarkYellow
                continue
            }
            Remove-Item -LiteralPath $target -Recurse -Force
        }

        if ($InstallMode -eq 'Link') {
            if (Test-Path -LiteralPath $item.Source -PathType Container) {
                # Junction не требует прав администратора, в отличие от SymbolicLink.
                New-Item -ItemType Junction -Path $target -Target $item.Source | Out-Null
            } else {
                New-Item -ItemType HardLink -Path $target -Target $item.Source | Out-Null
            }
            Write-Host "  соединение: $($item.Relative)" -ForegroundColor Green
        } else {
            Copy-Item -LiteralPath $item.Source -Destination $target -Recurse -Force
            Write-Host "  скопировано: $($item.Relative)" -ForegroundColor Green
        }
    }

    $skipped = @(Get-Items 'rules' '*.mdc') + @(Get-Items 'skills' '*') |
        Where-Object { $_.ProjectSpecific }
    if (-not $IncludeProjectSpecific -and $skipped) {
        Write-Host "`nОставлено в проекте (привязано к этому репозиторию):"
        $skipped | ForEach-Object { Write-Host "  $($_.Relative)" }
        Write-Host 'Перенести их тоже: ключ -IncludeProjectSpecific.'
    }

    Write-Host "`nГотово. Перезапустите Cursor или выполните Developer: Reload Window."
    Write-Host 'Проверка: откройте другой проект и наберите /planner в чате.'
}

function Build-Plugin {
    $pluginName = 'afar-calibration-agents'
    $dist = Join-Path $ProjectRoot "dist\cursor-plugin-$pluginName"

    if (Test-Path $dist) { Remove-Item -LiteralPath $dist -Recurse -Force }
    New-Item -ItemType Directory -Path (Join-Path $dist '.cursor-plugin') -Force | Out-Null

    foreach ($item in (Get-Selection)) {
        $groupDir = Join-Path $dist $item.Group
        New-Item -ItemType Directory -Path $groupDir -Force | Out-Null
        Copy-Item -LiteralPath $item.Source -Destination (Join-Path $groupDir $item.Name) -Recurse -Force
    }

    $manifest = [ordered]@{
        name        = $pluginName
        description = 'Субагенты, рабочие процессы и правила для проектов C++/Qt'
        version     = '1.0.0'
        author      = @{ name = $env:USERNAME }
    }
    $manifestPath = Join-Path $dist '.cursor-plugin\plugin.json'
    [System.IO.File]::WriteAllText(
        $manifestPath,
        ($manifest | ConvertTo-Json -Depth 5),
        (New-Object System.Text.UTF8Encoding $false))

    Write-Host "Плагин собран: $dist`n"
    Write-Host 'Локальная проверка на этой машине:'
    Write-Host "  Copy-Item '$dist' '$env:USERPROFILE\.cursor\plugins\local\$pluginName' -Recurse"
    Write-Host '  затем Developer: Reload Window и Customize в боковой панели.'
    Write-Host '  Соединения каталогов там не работают, нужна именно копия.'
    Write-Host "`nДля других машин: положите этот каталог в git-репозиторий и"
    Write-Host 'подключите его в Customize как источник плагинов.'
    Write-Host 'Хуки в плагин не попадают: путь к guard-shell.ps1 в нём не разрешается.'
}

function Uninstall-Items {
    $removed = 0
    foreach ($g in $Groups) {
        foreach ($item in (Get-Items $g.Name $g.Pattern)) {
            $target = Join-Path (Join-Path $UserRoot $g.Name) $item.Name
            if (Test-Path $target) {
                Remove-Item -LiteralPath $target -Recurse -Force
                Write-Host "  удалено: $($item.Relative)"
                $removed++
            }
        }
    }
    if ($removed -eq 0) { Write-Host 'Ничего не установлено — удалять нечего.' }
    else { Write-Host "`nУдалено объектов: $removed. Перезапустите Cursor." }
}

switch ($Mode) {
    'Status'    { Show-Status }
    'Copy'      { Install-Items 'Copy' }
    'Link'      { Install-Items 'Link' }
    'Plugin'    { Build-Plugin }
    'Uninstall' { Uninstall-Items }
}
