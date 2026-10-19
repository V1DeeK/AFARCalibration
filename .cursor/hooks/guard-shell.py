#!/usr/bin/env python3
# Хук beforeShellExecution Cursor для macOS и Linux (на Windows —
# guard-shell.ps1). Перед необратимыми командами git и рекурсивным удалением
# просит подтверждения пользователя. Не запрещает — спрашивает.
# stdin: JSON с полем "command". stdout: JSON с полем "permission".
# Правила — в guard-shell.json рядом, общие с guard-shell.ps1.
import json, os, re, sys

def decide(permission, user=None, agent=None):
    out = {"permission": permission}
    if user:
        out["user_message"] = user
    if agent:
        out["agent_message"] = agent
    sys.stdout.write(json.dumps(out, ensure_ascii=False))
    return 0

def main():
    try:
        command = json.load(sys.stdin).get("command", "")
        here = os.path.dirname(os.path.abspath(__file__))
        with open(os.path.join(here, "guard-shell.json"), encoding="utf-8") as f:
            cfg = json.load(f)
    except Exception:
        return decide("allow")  # сбой хука не блокирует работу
    for rule in cfg.get("rules", []):
        if re.search(rule["pattern"], command, re.IGNORECASE):
            return decide("ask",
                          cfg["messages"]["user"].format(rule["reason"]),
                          cfg["messages"]["agent"].format(rule["reason"]))
    return decide("allow")

sys.exit(main())
