# 시험용 출력 견본

| 파일 | 출처 |
|---|---|
| `ip_link_master_br0.json` | QEMU virt arm64 VM(Debian 13, iproute2 6.15.0)에서 `ip -j link show master br0` 실제 출력 (2026-10-26) |
| `bridge_vlan_show.json` | 같은 VM 에서 `bridge -j vlan show` 실제 출력. p3 에 30, p4 에 100-105 를 넣은 상태 |
| `bridge_vlan_show_compressed.json` | `bridge -c -j vlan show` 실제 출력. iproute2 6.15 에서 `-c` 는 색 옵션이라 범위로 묶이지 않는다(범위로 묶는 것은 `-compressvlans`) |
| `synthetic_vlan_range.json` | **직접 만든 것.** `vlanEnd` 가 있는 출력(묶어서 보여 주는 판)을 읽는 코드를 시험하려고 |
| `frr_running_two_routes.json` | 같은 VM, FRR 10.3-3+deb13u1 에서 `vtysh -c "show mgmt datastore-contents running json"` 실제 출력. 198.51.100.0/24 via 203.0.113.9, 198.51.100.128/25 blackhole |
| `frr_running_two_nexthops.json` | 위 상태에서 `ip route 198.51.100.0/24 203.0.113.10` 을 더한 뒤 실제 출력(한 prefix 에 next hop 둘) |
| `frr_running_empty.json` | 정적 경로를 모두 지운 뒤 실제 출력 |
| `frr_running_after_start.json` | 같은 VM 을 재부팅한 직후(정적 경로를 한 번도 설정하지 않은 FRR) 같은 명령의 실제 출력: `{}` 뿐 |

`upgrade/labsw-switch@2026-11-0{1,2,3}.yang`: 17장 시험용 가짜 새 revision (2026-10-27 판에서 한 군데씩 바꿈).
11-01 은 port 에 description 추가(호환), 11-02 는 VLAN name 길이 1..16(저장된 이름이 길면 거부), 11-03 은 VLAN name 삭제(데이터가 조용히 사라짐).
