#!/bin/zsh
# Claude Code hook: tells the board what this session is doing, so the eyes can react.
# Wired up in ~/.claude/settings.json for UserPromptSubmit, PreToolUse, PostToolUse, Notification, Stop, SessionEnd.
# Reads the hook's JSON on stdin and posts {"session","state","detail","project","agent","model","title","tool","t"} to http://bobble.local/agent.
# Never blocks or fails Claude: the post runs in the background and every error is swallowed.
#   AGENT_PET_HOST=192.168.68.62  to use an IP if .local names don't resolve
in=$(cat)
# every event's raw input, one per line (last ~200 kept), for working out odd ones later
dbg="${TMPDIR:-/tmp}/agent_pet_events.jsonl"
print -r -- "${in//$'\n'/ }" >> "$dbg" 2>/dev/null
[[ $(wc -l < "$dbg" 2>/dev/null) -gt 400 ]] && { tail -n 200 "$dbg" > "$dbg.tmp" && mv "$dbg.tmp" "$dbg"; } 2>/dev/null
dir="${0:A:h}"
host=${AGENT_PET_HOST:-bobble.local}
token=$(sed -n 's/.*OTA_TOKEN "\(.*\)".*/\1/p' "$dir/main/secrets.h")

j() { print -r -- "$in" | jq -r "$1 // empty" 2>/dev/null }
event=$(j .hook_event_name)
tool=$(j .tool_name)
session=$(j .session_id)
file=$(j '.tool_input.file_path // .tool_input.notebook_path')
file=${file:t}
project=$(j .cwd)
# the app it's in, for its logo: Conductor runs each chat in a workspace folder of its own
app=claude
[[ $project == */conductor/workspaces/* ]] && app=conductor
project=${project:t}
# what the last tool was about to do, as a question: a permission prompt that follows shows it ("Run git push?")
ask_file="${TMPDIR:-/tmp}/agent_pet_${session//[^A-Za-z0-9-]/}"

# The model, as the board shows it: claude-opus-5-5 -> "Opus 5.5" (claude-3-5-sonnet-2024... -> "Sonnet 3.5").
# Read from the transcript when a turn starts or ends, remembered for the session in between.
model_file="$ask_file.model"
pretty_model() {
    local fam= ver= p
    for p in ${(s:-:)${${1#claude-}%%\[*}}; do        # ([1m]: the long-context variant, same model)
        if [[ $p == <-> ]]; then (( ${#p} <= 2 )) && ver+="${ver:+.}$p"
        elif [[ -z $fam ]]; then fam=${(C)p}
        fi
    done
    print -r -- "$fam${ver:+ $ver}"
}
model=$(j ".model | strings")
if [[ -z $model && ( $event == UserPromptSubmit || $event == Stop || ! -s $model_file ) ]]; then
    model=$(tail -n 60 "$(j .transcript_path)" 2>/dev/null | jq -r 'select(.type == "assistant") | .message.model // empty' 2>/dev/null | tail -n 1)
fi
if [[ -n $model ]]; then model=$(pretty_model "$model"); print -r -- "$model" > "$model_file" 2>/dev/null
else model=$(cat "$model_file" 2>/dev/null); fi

# The program a shell command runs, for a command without a description: skips "cd dir &&", VAR=value, sudo/env/time
# and quotes ("cd app && FOO=1 npm test" -> npm); a loop, function or other shell construct is just "a script"
cmd_name() {
    local -a w=(${(z)${1##[[:space:]]#}})
    local i=1
    while (( i <= $#w )); do
        case ${w[i]} in
            cd|pushd) (( i += 2 )); [[ ${w[i]} == ('&&'|';'|'||') ]] && (( i++ )); continue ;;
            ';'|'&&'|'||'|sudo|time|env|nohup|command|exec) (( i++ )); continue ;;
            [A-Za-z_]*=*) (( i++ )); continue ;;
            -*) (( i++ )); continue ;;              # (sudo -n ...: options before the program)
        esac
        break
    done
    local n=${(Q)w[i]}
    case $n in
        for|while|until|if|case|select|function|'{'|'('|'(('|'[['|*'()') print -r -- "a script"; return ;;
    esac
    [[ ${w[i+1]} == '()' || ${w[i+1]} == '(' ]] && { print -r -- "a script"; return }   # (a function definition)
    print -r -- "${${n:t}:-${(Q)w[1]}}"
}

# a tool call after the turn has finished (and no new prompt) is Claude Code's own housekeeping, not your session
# working again: leave the board on "done"
turn_file="$ask_file.turn"
case $event in
    UserPromptSubmit) print active > "$turn_file" 2>/dev/null; rm -f "$detail_file" ;;
    Stop)             print done > "$turn_file" 2>/dev/null ;;
    PreToolUse|PostToolUse) [[ $(cat "$turn_file" 2>/dev/null) == done ]] && exit 0 ;;
esac

# What Claude last said to you in this turn ("Now the firmware: every new message wakes the screen."), as plain
# text: its first sentence or two, up to ~140 characters. Empty if it hasn't said anything since your prompt, or
# has run more than 2 tools since (it's moved on; and the transcript doesn't always have its latest words yet).
# Some of Claude's text as one plain line: the first line, or the first point after an intro like "Two things:"
# (a line ending in a colon), markdown and list numbers stripped, whole sentences up to ~140 characters
summarize() {
    local t
    t=$(print -r -- "$1" | awk 'NF { n++; l[n] = $0 } END { i = (n > 1 && l[1] ~ /:[[:space:]]*$/) ? 2 : 1; print l[i] }' |
        sed -E 's/[*`#_]//g; s/\[([^]]*)\]\([^)]*\)/\1/g; s/^[[:space:]>-]+//; s/^[0-9]+[.)] +//; s/[[:space:]]+/ /g; s/:$//')
    if (( ${#t} > 140 )); then
        local cut=${t[1,140]}
        if [[ $cut == *[.!?]' '* ]]; then t=${cut%[.!?] *}${${cut##*[.!?] }:+}; t=${cut[1,${#t}+1]}   # whole sentences
        else t="${cut% *}..."; fi
    fi
    print -r -- "$t"
}

narration() {
    local t
    t=$(tail -n 80 "$(j .transcript_path)" 2>/dev/null | jq -rs '
        (map(.type == "user" and ((.message.content | type) == "string" or ([.message.content[]? | .type] | index("text") != null))) | rindex(true)) as $u
        | [to_entries[] | select(.key > ($u // -1)) | .value | select(.type == "assistant") | .message.content[]?
           | select(.type == "text" or .type == "tool_use")]
        | (map(.type == "text") | rindex(true)) as $k
        | if $k == null or (length - $k - 1) > 2 then empty else .[$k].text end' 2>/dev/null)   # (stale after 3+ tools: show the action)
    summarize "$t"
}
detail_file="$ask_file.detail"   # the last working message, put back after a tool (or a permission prompt) finishes

# The chat's title, as Claude Code names it in the transcript ("Access to other chats"); looked up when a turn starts
# or ends, remembered in between
title_file="$ask_file.title"
if [[ $event == (UserPromptSubmit|Stop) || ! -s $title_file ]]; then
    title=$(grep -h '"type":"ai-title"' "$(j .transcript_path)" 2>/dev/null | tail -n 1 | jq -r '.aiTitle // empty' 2>/dev/null)
    [[ -n $title ]] && print -r -- "$title" > "$title_file" 2>/dev/null
fi
title=$(cat "$title_file" 2>/dev/null | LC_ALL=C tr -cd '\40-\176')

# Which integration a tool belongs to, as the board knows its logos: mcp__claude_ai_Google_Calendar__list -> googlecalendar
tool_file="$ask_file.tool"
integration() {
    [[ $1 == mcp__* ]] || return
    local k=${${${1#mcp__}%%__*}#claude_ai_}
    k=${(L)k//[^A-Za-z0-9]/}
    case $k in
        googlecloudbigquery) k=googlebigquery ;;
        googledocs|googlesheets|googleslides) k=googledrive ;;
    esac
    print -r -- $k
}
itool=

state=working detail= ask=
case $event in
    UserPromptSubmit) detail="Thinking"
                      summarize "$(j .prompt)" > "$ask_file.prompt" 2>/dev/null ;;   # (for "Finished: ..." if the reply says nothing)
    PostToolUse)      detail=$(cat "$detail_file" 2>/dev/null); [[ -n $detail ]] || detail="Thinking"
                      itool=$(cat "$tool_file" 2>/dev/null) ;;
    PreToolUse)
        itool=$(integration "$tool")
        print -r -- "$itool" > "$tool_file" 2>/dev/null
        case $tool in
            Edit|Write|MultiEdit|NotebookEdit) detail="Editing $file" ask="Edit $file?" ;;
            Read)            detail="Reading $file" ask="Read $file?" ;;
            Bash)            cmd=$(j .tool_input.command); detail=$(j .tool_input.description)   # Claude's own words for it
                             [[ -n $detail ]] || detail="Running $(cmd_name "$cmd")"
                             ask="Run ${${cmd%%$'\n'*}[1,100]}?" ;;
            Grep|Glob)       detail="Searching" ;;
            WebFetch)        detail="Browsing the web" ask="Open ${$(j .tool_input.url)[1,90]}?" ;;
            WebSearch)       detail="Browsing the web" ;;
            Task|Agent)      detail="Delegating" ;;
            TodoWrite)       detail="Planning" ;;
            AskUserQuestion) state=waiting detail=$(j '.tool_input.questions[0].question') ;;
            mcp__*)          detail="Using ${${${${tool#mcp__}%%__*}#claude_ai_}//_/ }" ;;
            *)               detail="Using $tool" ;;
        esac
        [[ -n $ask ]] || ask="${detail:-Use $tool}?"
        print -r -- "$ask" > "$ask_file" 2>/dev/null
        if [[ $state == working ]]; then                 # what Claude says it's doing beats the bare action
            said=$(narration)
            [[ -n $said ]] && detail=$said
            print -r -- "$detail" > "$detail_file" 2>/dev/null
        fi ;;
    Notification)
        # permission prompts and questions need you; the "idle for 60 s" reminder doesn't (it's already shown as done)
        case "$(j .notification_type)" in
            permission_prompt) state=waiting detail=$(cat "$ask_file" 2>/dev/null) ;;
            elicitation_dialog) state=waiting detail=$(j .message) ;;
            idle_prompt|auth_success) exit 0 ;;
            *) [[ "$(j .message)" == *ermission* ]] || exit 0
               state=waiting detail=$(cat "$ask_file" 2>/dev/null) ;;
        esac
        [[ -n $detail ]] || detail="Needs your permission"
        itool=$(cat "$tool_file" 2>/dev/null) ;;          # (asking about a Linear call: still the Linear logo)
    Stop)
        # what it did: the start of Claude's final reply (Claude Code hands it over; the transcript may lag), else
        # what you'd asked for
        state=done
        detail=$(summarize "$(j .last_assistant_message)")
        if [[ -z $detail || $detail == (Done|Done.|done) ]]; then
            p=$(cat "$ask_file.prompt" 2>/dev/null)
            detail=${p:+Finished: $p}
        fi
        [[ -n $detail ]] || detail="Finished" ;;
    SessionEnd) state=idle; rm -f "$ask_file" "$model_file" "$turn_file" "$detail_file" "$ask_file.prompt" "$ask_file.title" "$tool_file" ;;
    *) exit 0 ;;
esac

detail=$(print -r -- "$detail" | LC_ALL=C tr -cd '\40-\176')   # the board's font is plain ASCII
(( ${#detail} > 190 )) && detail="${${detail[1,190]}% *}..."   # (the board keeps up to 199 characters: end on a whole word)
zmodload zsh/datetime
body=$(jq -nc --arg s "$session" --arg st "$state" --arg d "$detail" --arg p "$project" --arg m "$model" --arg ti "$title" --arg it "$itool" --arg app "$app" \
          --argjson t "${${EPOCHREALTIME/./}[1,13]}" '{session:$s,state:$st,detail:$d,project:$p,agent:$app,provider:"claude",model:$m,title:$ti,tool:$it,t:$t}')
# (-4: a .local lookup tries IPv6 first and stalls ~7 s)
curl -4 --silent --max-time 8 -H "X-OTA-Token: $token" -H "Content-Type: application/json" \
     --data "$body" "http://$host/agent" > /dev/null 2>&1 &!
exit 0
