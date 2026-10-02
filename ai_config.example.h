// -----------------------------------------------------------------------------
// Template for ai_config.h — copy this file to ai_config.h and fill in your key.
// ai_config.h is gitignored so the key never lands in the repo.
//
//   cp ai_config.example.h ai_config.h
//
// Get a key at https://console.groq.com/keys
// -----------------------------------------------------------------------------
#ifndef AI_CONFIG_H
#define AI_CONFIG_H

#define GROQ_API_KEY "PUT_YOUR_GROQ_API_KEY_HERE"

// Must be a chat model that returns text directly in choices[0].message.content.
// Reasoning models (openai/gpt-oss-*) return an empty content with the budget
// spent on a separate `reasoning` field, and qwen3.x emits <think> blocks inline.
#define GROQ_MODEL "groq/compound-mini"

#endif
