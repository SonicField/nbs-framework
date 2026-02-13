# Chat Search

**Requested by:** Alex Turner
**Date:** 12 February 2026
**Priority:** Nice-to-have

## Description

Add a `/search` command to `nbs-chat-terminal` that enters search mode, allowing the user to search message history by keyword or pattern.

## Motivation

In long chat sessions with multiple participants, finding a specific earlier message (e.g. a question, a decision, a specific @mention) requires scrolling through the entire history. A search feature would make this tractable.

## Possible implementation

1. Add `/search <pattern>` command to `nbs-chat-terminal`
2. When invoked, display matching messages with context (handle, timestamp, surrounding messages)
3. Support basic substring matching and optionally regex
4. Consider `nbs-chat search <file> <pattern>` as a CLI command too (useful for AI agents and scripts)

## Notes

- The base64-decoded message content is searchable — decode all messages, grep pattern
- Should show handle and message index for each match
- Could support `--handle=<name>` filter to search only one participant's messages
