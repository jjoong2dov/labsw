# 다시 만들기(replay) 기록

책의 12·16·17장 기능을 "그 기능을 넣기 직전 상태"에서 Claude Code 에게 요구사항 문서 같은 프롬프트로 시킨 실제 기록입니다.

| 장 | 시작 상태(태그) | 세션이 만든 판(가지) | 프롬프트 |
|---|---|---|---|
| 12 운용 상태와 알림 | `ch12-start` | `replay/ch12` | `prompts/ch12.txt` |
| 16 백업·복원 RPC | `ch16-start` | `replay/ch16` | `prompts/ch16.txt` |
| 17 스키마 업그레이드 | `ch17-start` | `replay/ch17` | `prompts/ch17.txt` |

저자는 기록을 남기려고 `claude -p`(허용 명령은 `prompts/allowed.txt`)로 돌렸습니다. 대화 화면에 같은 프롬프트를 넣어도 됩니다. 여러분의 결과는 책과 다르게 나옵니다. 같아야 할 것은 판정 시험의 통과입니다.

해 보기:

```sh
git switch -c my-ch16 ch16-start
# claude 에게 prompts/ch16.txt 를 그대로 주고, 끝나면 커밋
tools/replay_judge.py ch16 my-ch16          # 그 장 완성판의 시험을 여러분의 판에 돌림 (results/judge-*)
tools/replay_judge.py ch16                  # 책의 replay 판(replay/ch16)
```

판정기는 세션이 보지 못한 시험(그 장 완성 커밋의 통합 시험)을 얹어 돌리고, 책의 구현 문구까지 보는 단언은 프롬프트가 요구한 수준으로 느슨하게 합니다(`tools/replay_judge.py` 의 RELAX). 책 원고는 12.10절, 16.10절, 17.11절과 부록 C 를 보십시오.
