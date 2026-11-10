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

// OPTIONAL: a few facts about the watch's owner so the chatbot feels personal. Put the real one in your
// own (gitignored) ai_config.h, never in the repo. It is sent to Groq with every message, so keep it light:
// no address, phone number or other people's full names. One plain-ASCII string, no double quotes inside,
// and no full stop at the end. Leave it out and the chatbot is just as friendly, only less personal.
// #define AI_OWNER_PROFILE "Their name is Alex. They like football and coding, and prefer short, honest answers"

#endif
