# PA3 로컬 채점기

PA3의 각 step에서 가장 기초적인 항목만 확인하는 채점기다. 실제 채점은 Gradescope에서 훨씬 많은 항목으로 하므로, 여기를 통과했다고 만점이 보장되지는 않는다. 각 step 문서의 "확인" 절에 나온 것도 직접 해 봐야 한다.

## 준비 (처음 한 번)

```bash
pip3 install --user gradescope-utils
```

## 실행

```bash
cd ~/2026-2-data-engineering/pa3/local-grader     # 레포를 받은 위치에 맞게
./run_local.sh $PGBASE/postgres-18
```

- 소스 트리를 default 빌드와 snudbx 빌드로 `/tmp/pa3-work` 아래에 새로 빌드하고(`$PGBASE`의 빌드·설치본과는 별개), DB를 만들고, 테스트를 실행한다. 작업 경로는 `PA3_WORK`로 바꿀 수 있다.
- 처음 실행은 빌드 때문에 몇 분 걸린다. 다음부터는 바뀐 파일만 다시 컴파일한다.
- 아직 구현하지 않은 step의 항목은 실패로 표시된다. 지금 작업 중인 step까지만 통과하면 정상이다.
- 실패한 항목 아래에는 이유가 함께 출력된다. 빌드 항목(0.1, 0.2)이 실패하면 나머지는 의미가 없으므로 그것부터 고친다.
- 채점기는 flush worker에 `SIGTERM`을 보내 재시작을 확인한다(1.2). 서버는 채점기가 따로 띄운 것이므로 작업 중인 서버에는 영향이 없다.

## 들어 있는 항목

번호는 `<step>.<항목>`이다. 1.x는 Step 3-1, 2.x는 Step 3-2, 3.x는 Step 3-3, 4.x는 Step 3-4.

| Step | 번호 | 확인 내용 |
|---|---|---|
| 공통 | 0.1, 0.2 | default 빌드와 snudbx 빌드가 컴파일된다 |
| 3-1 | 1.1 | `snudbx flush worker` 프로세스가 `ps`와 `pg_stat_activity`에 하나 있다 |
| 3-1 | 1.2 | worker에 `SIGTERM`을 보내면 3초 안에 새 worker가 뜨고, 서버는 그대로다 |
| 3-2 | 2.1 | `contrib/snudbx`에 Step 3-2의 함수 3개(`snudbx_sealed_pool`, `snudbx_pool_state`, `snudbx_flush_worker_pid`)가 맞는 시그니처로 있다 |
| 3-2 | 2.2 | 시작 직후 상태: worker pid, SEALED 풀 없음, 풀 상태 `ACTIVE/ACTIVE/READY` |
| 3-2 | 2.3 | PA2 Step 2-3의 숫자(715, 1429, seal)가 그대로이고, seal된 풀이 비워지며, 데이터가 맞다 |
| 3-3 | 3.1 | INSERT만으로 seal된 풀을 worker가 쓰고 비운다(`pg_stat_io`), CHECKPOINT 뒤 crash에도 데이터가 남는다 |
| 3-3 | 3.2 | 90만 행(seal 여러 번)이 오류 없이 들어가고, worker가 재시작하지 않고, SEALED로 남은 풀이 없고, sealed pool을 디스크에 쓴 것은 worker다 |
| 3-3 | 3.3 | INSERT만 하는 동안 backend는 페이지를 직접 쓰지 않는다(`pg_stat_io`의 client backend writes. Step 3-4부터는 seal에 걸친 페이지를 복사하느라 seal마다 1까지 쓸 수 있다) |
| 3-4 | 4.1 | hot-update 워크로드(`pgbench`): 갱신이 사라지지 않고, worker가 재시작하지 않고(sealed 페이지를 제자리에서 고치면 reset이 실패한다), 모든 seal이 reset되고, backend는 복사할 때만 몇 페이지 쓴다 |

새 step이 나오면 항목이 추가된다. `git pull`로 받는다.
