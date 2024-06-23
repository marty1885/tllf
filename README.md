# TLLF - Totally Legit Language Framework

Lightweight C++ framework for building applications with Large Language Models. Currently it is a pile of glue bridging language model services into C++.

This is work in progress software. A lot of ideas comes from LangChain. Not it is not a 1 to 1 copy. I'm adding and modifying things to fit my needs.

Features:

* Support OpenAI style API endpoints
* Support Google Gemini (VertexAI) API endpoints
* Supports multi-modal inputs
* Basic prompt templating
* Basic response parsing

## TODOs:

- [x] More general input API
- [ ] More general output API
- [ ] Some framework to automatically retry with larger LLMs if as task fails