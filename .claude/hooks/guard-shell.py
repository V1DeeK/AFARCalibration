#!/usr/bin/env python3
# Хук PreToolUse для Bash: перед необратимыми командами git и рекурсивным
# удалением просит подтверждения пользователя. Не запрещает — спрашивает.
# Правила — в guard-shell.json рядом. Перенесено из .cursor/hooks/guard-shell.ps1.
import json, os, re, sys

def main():
    try:
        data = json.load(sys.stdin)
        command = data.get("tool_input", {}).get("command", "")
        here = os.path.dirname(os.path.abspath(__file__))
        with open(os.path.join(here, "guard-shell.json"), encoding="utf-8") as f:
            cfg = json.load(f)
    except Exception:
        return 0  # как failClosed: false в Cursor — сбой хука не блокирует работу
    for rule in cfg.get("rules", []):
        if re.search(rule["pattern"], command, re.IGNORECASE):
            reason = cfg["messages"]["user"].format(rule["reason"])
            print(json.dumps({"hookSpecificOutput": {
                "hookEventName": "PreToolUse",
                "permissionDecision": "ask",
                "permissionDecisionReason": reason,
            }}, ensure_ascii=False))
            return 0
    return 0

sys.exit(main())
