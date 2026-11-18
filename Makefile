.PHONY: sim-minimal sim-fixed-mo5 sim-fixed-mo6 sim-adaptive analyze-qos compare-qos validate-adaptive
sim-minimal:
	./tools/run-dsme-simulation minimal
sim-fixed-mo5:
	./tools/run-dsme-simulation fixed-mo5
sim-fixed-mo6:
	./tools/run-dsme-simulation fixed-mo6
sim-adaptive:
	./tools/run-dsme-simulation adaptive
analyze-qos:
	@test -n "$(LOG)" || (echo 'usage: make analyze-qos LOG=path/to/log' >&2; exit 2)
	python3 tools/cooja/codespaces/analyze_qos_metrics.py "$(LOG)"
compare-qos:
	python3 tools/cooja/codespaces/compare_qos_metrics.py
validate-adaptive:
	@test -n "$(LOG)" || (echo 'usage: make validate-adaptive LOG=simulation-results/path-to-adaptive.log' >&2; exit 2)
	@report="simulation-results/$$(date -u +%Y%m%dT%H%M%S.%NZ)-$$$$-adaptive-validation-only.txt"; \
	python3 tools/cooja/codespaces/validate_simulation.py --mode adaptive \
	  --scenario examples/ipv6/rpl-udp-dsme/dsme-adaptive-mo.csc \
	  --log "$(LOG)" --report "$$report"; status=$$?; echo "VALIDATION_REPORT=$$report"; exit $$status
