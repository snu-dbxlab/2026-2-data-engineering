# PA2 로컬 채점기

PA2의 각 step에서 가장 기초적인 항목만 확인하는 채점기다. 실제 채점은 Gradescope에서 훨씬 많은 항목으로 하므로, 여기를 통과했다고 만점이 보장되지는 않는다. 각 step 문서의 "테스트" 절에 나온 확인도 직접 해 봐야 한다.

## 준비 (처음 한 번)

```bash
pip3 install --user gradescope-utils
```

## 실행

```bash
cd ~/2026-2-data-engineering/pa2/local-grader     # 레포를 받은 위치에 맞게
./run_local.sh $PGBASE/postgres-18
```

- 소스 트리를 default 빌드와 snudbx 빌드로 `/tmp/pa2-work` 아래에 새로 빌드하고(`$PGBASE`의 빌드·설치본과는 별개), DB를 만들고, 테스트를 실행한다. 작업 경로는 `PA2_WORK`로 바꿀 수 있다.
- 처음 실행은 빌드 때문에 몇 분 걸린다. 다음부터는 바뀐 파일만 다시 컴파일한다.
- 아직 구현하지 않은 step의 항목은 실패로 표시된다. 지금 작업 중인 step까지만 통과하면 정상이다.
- 실패한 항목 아래에는 이유가 함께 출력된다. 빌드 항목(0.1, 0.2)이 실패하면 나머지는 의미가 없으므로 그것부터 고친다.

## 들어 있는 항목

| Step | 번호 | 확인 내용 |
|---|---|---|
| 공통 | 0.1, 0.2 | default 빌드와 snudbx 빌드가 컴파일된다 |
| 1-1 | 1.1 | `buffer_pools`의 타입, context, 범위, 기본값 |
| 1-1 | 1.5 | `buffer_pools` 3, 4, 8로 서버가 뜬다 |
| 1-2 | 2.1 | `Buffer Pool Control` 영역이 있다 |
| 1-2 | 2.4 | `contrib/snudbx` 확장 모듈과 함수 시그니처 |
| 1-3 | 3.1 | 풀별 영역 6종이 풀 수만큼 있다 |

새 step이 나오면 항목이 추가된다. `git pull`로 받는다.
