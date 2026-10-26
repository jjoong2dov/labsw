# 시험용 출력 견본

| 파일 | 출처 |
|---|---|
| `ip_link_master_br0.json` | QEMU virt arm64 VM(Debian 13, iproute2 6.15.0)에서 `ip -j link show master br0` 실제 출력 (2026-10-26) |
| `bridge_vlan_show.json` | 같은 VM 에서 `bridge -j vlan show` 실제 출력. p3 에 30, p4 에 100-105 를 넣은 상태 |
| `bridge_vlan_show_compressed.json` | `bridge -c -j vlan show` 실제 출력. iproute2 6.15 의 JSON 은 `-c` 를 줘도 범위로 묶지 않는다 |
| `synthetic_vlan_range.json` | **직접 만든 것.** `vlanEnd` 가 있는 출력(묶어서 보여 주는 판)을 읽는 코드를 시험하려고 |
