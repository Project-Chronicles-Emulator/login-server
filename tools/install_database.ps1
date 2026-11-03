#requires -Version 7.2
<#
.SYNOPSIS
Creates the Project Chronicles database and installs its complete schema.
.DESCRIPTION
Requires a running PostgreSQL 16 server and psql. Passwords are prompted securely,
or supplied as SecureString parameters. Existing roles keep their passwords.
An existing current database is preserved; older or unrelated schemas are rejected.
.EXAMPLE
pwsh -File tools/install_database.ps1
.EXAMPLE
pwsh -File tools/install_database.ps1 -PostgresBin 'C:\Program Files\PostgreSQL\16\bin'
#>
[CmdletBinding()]
param(
    [ValidatePattern('^[A-Za-z0-9_.:-]+$')]
    [string]$DatabaseHost = '127.0.0.1',
    [ValidateRange(1, 65535)]
    [int]$DatabasePort = 5432,
    [ValidatePattern('^[a-z][a-z0-9_]{0,62}$')]
    [string]$DatabaseName = 'chronicles',
    [ValidatePattern('^[a-z][a-z0-9_]{0,62}$')]
    [string]$DatabaseUser = 'chronicles',
    [ValidatePattern('^[a-z][a-z0-9_]{0,62}$')]
    [string]$AdminUser = 'postgres',
    [Security.SecureString]$AdminPassword,
    [Security.SecureString]$DatabasePassword,
    [string]$PostgresBin
)

$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$schemaPath = Join-Path $projectRoot 'migrations/001_accounts_characters.sql'

function Find-Psql {
    if ($PostgresBin) {
        $name = if ($IsWindows) { 'psql.exe' } else { 'psql' }
        return (Resolve-Path -LiteralPath (Join-Path $PostgresBin $name)).Path
    }

    $command = Get-Command psql -CommandType Application -ErrorAction SilentlyContinue
    if ($command) {
        return $command.Source
    }

    $candidates = @(Join-Path $projectRoot 'out/postgresql-tools/pgsql/bin/psql.exe')
    if ($IsWindows -and $env:ProgramFiles) {
        $candidates += Join-Path $env:ProgramFiles 'PostgreSQL/16/bin/psql.exe'
    }
    foreach ($candidate in $candidates) {
        if (Test-Path -LiteralPath $candidate -PathType Leaf) {
            return (Resolve-Path -LiteralPath $candidate).Path
        }
    }

    throw 'Install PostgreSQL 16 and add its bin directory to PATH, or pass -PostgresBin.'
}

function ConvertFrom-PrivatePassword {
    param([Security.SecureString]$Password)

    $pointer = [Runtime.InteropServices.Marshal]::SecureStringToBSTR($Password)
    try {
        return [Runtime.InteropServices.Marshal]::PtrToStringBSTR($pointer)
    } finally {
        [Runtime.InteropServices.Marshal]::ZeroFreeBSTR($pointer)
    }
}

function Invoke-PostgresSql {
    param(
        [string]$Database,
        [string]$User,
        [Security.SecureString]$Password,
        [string]$Sql,
        [switch]$Sensitive
    )

    $startInfo = [Diagnostics.ProcessStartInfo]::new()
    $startInfo.FileName = $script:psqlPath
    $startInfo.UseShellExecute = $false
    $startInfo.CreateNoWindow = $true
    $startInfo.RedirectStandardInput = $true
    $startInfo.RedirectStandardOutput = $true
    $startInfo.RedirectStandardError = $true
    $startInfo.StandardInputEncoding = [Text.UTF8Encoding]::new($false)
    $startInfo.StandardOutputEncoding = [Text.UTF8Encoding]::new($false)
    $startInfo.StandardErrorEncoding = [Text.UTF8Encoding]::new($false)
    foreach ($argument in @('-X', '--no-password', '--quiet', '--tuples-only', '--no-align',
            '--set', 'ON_ERROR_STOP=1', '--host', $DatabaseHost,
            '--port', [string]$DatabasePort, '--username', $User, '--dbname', $Database)) {
        $startInfo.ArgumentList.Add($argument)
    }
    $startInfo.Environment['PGCONNECT_TIMEOUT'] = '10'
    $startInfo.Environment['PGCLIENTENCODING'] = 'UTF8'
    if ($null -ne $Password) {
        $startInfo.Environment['PGPASSWORD'] = ConvertFrom-PrivatePassword $Password
    }

    $process = [Diagnostics.Process]::new()
    $process.StartInfo = $startInfo
    try {
        if (-not $process.Start()) {
            throw 'Could not start psql.'
        }
        $stdout = $process.StandardOutput.ReadToEndAsync()
        $stderr = $process.StandardError.ReadToEndAsync()
        $process.StandardInput.WriteLine($Sql)
        $process.StandardInput.Close()
        $process.WaitForExit()
        $output = $stdout.GetAwaiter().GetResult().Trim()
        $errorOutput = $stderr.GetAwaiter().GetResult().Trim()
        if ($process.ExitCode -ne 0) {
            if ($Sensitive) {
                throw 'PostgreSQL role creation failed. Check administrator privileges and password policy.'
            }
            throw "PostgreSQL failed for database '$Database': $errorOutput"
        }
        return $output
    } finally {
        $startInfo.Environment.Remove('PGPASSWORD') | Out-Null
        $process.Dispose()
    }
}

if ($DatabaseUser -eq $AdminUser) {
    throw 'Choose a dedicated DatabaseUser different from AdminUser.'
}
if (-not (Test-Path -LiteralPath $schemaPath -PathType Leaf)) {
    throw "The complete schema was not found: $schemaPath"
}
$script:psqlPath = Find-Psql

if ($null -eq $AdminPassword -and -not $env:PGPASSWORD -and -not $env:PGPASSFILE) {
    $AdminPassword = Read-Host "PostgreSQL administrator password ($AdminUser)" -AsSecureString
}
$adminConnection = @{
    Database = 'postgres'
    User = $AdminUser
    Password = $AdminPassword
}
$version = Invoke-PostgresSql @adminConnection -Sql 'SHOW server_version_num;'
if ([int]$version -lt 160000 -or [int]$version -ge 170000) {
    throw "PostgreSQL 16 is required; the server reported version $version."
}

# Identifier parameters are restricted to simple lower-case SQL identifiers above.
$owner = Invoke-PostgresSql @adminConnection -Sql @"
SELECT pg_get_userbyid(datdba) FROM pg_database WHERE datname = '$DatabaseName';
"@
if ($owner -and $owner -ne $DatabaseUser) {
    throw "Database '$DatabaseName' belongs to '$owner'. Choose another database name."
}
$role = Invoke-PostgresSql @adminConnection -Sql @"
SELECT CASE WHEN rolcanlogin AND NOT rolsuper AND NOT rolcreaterole
    AND NOT rolcreatedb AND NOT rolreplication AND NOT rolbypassrls
    THEN 'application' ELSE 'privileged_or_disabled' END
FROM pg_roles WHERE rolname = '$DatabaseUser';
"@
if ($role -and $role -ne 'application') {
    throw "Role '$DatabaseUser' is not a dedicated application login. Choose another DatabaseUser."
}
if ($null -eq $DatabasePassword) {
    $prompt = if ($role) { 'Existing database password' } else { 'New database password (12+ characters)' }
    $DatabasePassword = Read-Host "$prompt ($DatabaseUser)" -AsSecureString
}
if (-not $role -and $DatabasePassword.Length -lt 12) {
    throw 'Use at least 12 characters for the new database password.'
}

if (-not $role) {
    $plainPassword = ConvertFrom-PrivatePassword $DatabasePassword
    try {
        if ($plainPassword.Contains([char]0) -or $plainPassword.Contains("`n") -or
                $plainPassword.Contains("`r")) {
            throw 'Database passwords cannot contain NUL or line-break characters.'
        }
        $passwordLiteral = $plainPassword.Replace("'", "''")
        $roleSql = @"
SET standard_conforming_strings = on;
CREATE ROLE "$DatabaseUser" LOGIN NOSUPERUSER NOCREATEDB NOCREATEROLE
    NOREPLICATION NOBYPASSRLS PASSWORD '$passwordLiteral';
"@
        Invoke-PostgresSql @adminConnection -Sql $roleSql -Sensitive | Out-Null
    } finally {
        $plainPassword = $null
        $passwordLiteral = $null
        $roleSql = $null
    }
}
Invoke-PostgresSql -Database 'postgres' -User $DatabaseUser `
    -Password $DatabasePassword -Sql 'SELECT 1;' | Out-Null
if (-not $owner) {
    Invoke-PostgresSql @adminConnection -Sql @"
CREATE DATABASE "$DatabaseName" OWNER "$DatabaseUser" TEMPLATE template0 ENCODING 'UTF8';
"@ | Out-Null
}

$schema = Get-Content -LiteralPath $schemaPath -Raw -Encoding utf8
Invoke-PostgresSql -Database $DatabaseName -User $DatabaseUser `
    -Password $DatabasePassword -Sql $schema | Out-Null

Write-Host "Database '$DatabaseName' is ready on ${DatabaseHost}:$DatabasePort."
Write-Host 'Use these settings in the terminal that starts the server:'
Write-Output "`$env:CHRONICLES_DATABASE_URL = 'host=$DatabaseHost port=$DatabasePort dbname=$DatabaseName user=$DatabaseUser connect_timeout=5'"
Write-Output "`$env:PGPASSWORD = Read-Host 'Database password' -MaskInput"
