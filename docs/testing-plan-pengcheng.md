# Initial Network Project Test Plan

Proposed by: Peng Cheng
Status: Draft for team discussion

## Purpose

Define an initial testing scope for our CS374 group project.
The final test cases will be updated after the topic and
module responsibilities are agreed upon.

## Proposed Tests

| ID | Scenario | Expected result |
| --- | --- | --- |
| T01 | A client connects to the server | The connection succeeds and the server records it |
| T02 | Four clients perform supported operations concurrently | Responses reach the correct clients without data mixing |
| T03 | A client disconnects unexpectedly | The server remains available to other clients and releases resources |
| T04 | A client sends an invalid application message | The server handles it without crashing |
| T05 | A client sends data exceeding the documented size limit | The server rejects it safely |
| T06 | Clients repeatedly connect and disconnect | Open descriptors and memory do not grow continuously due to leaks |
| T07 | A supported operation runs between two computers | The result matches the documented behavior |

## Evidence to Collect

- Build and execution commands
- Operating system and network setup
- Expected and actual results
- Relevant logs and packet captures
- Commit hash of the tested version

## Questions for the Team

- Which project topic will we implement?
- What application protocol and message size limits will we use?
- Who will own each implementation module and its tests?
- What are the presentation and report deadlines?

## Current Progress

This document proposes a testing plan only.
No implementation or test execution is claimed.
