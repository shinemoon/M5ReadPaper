param(
    [string]$Command,
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]]$CommandArgs
)

# Workspace shell bootstrap script.
# When VS Code tasks append "-Command ...", execute that command here.
if ($Command) {
    & $Command @CommandArgs
}
