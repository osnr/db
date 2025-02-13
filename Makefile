ifeq ($(shell uname -s),Linux)
	override CFLAGS += -Wl,--export-dynamic
endif

ifneq (,$(filter -DTRACY_ENABLE,$(CFLAGS)))
# Tracy is enabled
	TRACY_TARGET = vendor/tracy/public/TracyClient.o
	override CPPFLAGS += -std=c++20 -DTRACY_ENABLE
	LINKER := c++
else
	TRACY_CFLAGS :=
	LINKER := cc
endif

folk: workqueue.o db.o trie.o sysmon.o epoch.o folk.o \
	vendor/c11-queues/mpmc_queue.o vendor/c11-queues/memory.o \
	vendor/jimtcl/libjim.a $(TRACY_TARGET)

	$(LINKER) -g -fno-omit-frame-pointer -o$@ \
		$(CFLAGS) $(TRACY_CFLAGS) \
		-L./vendor/jimtcl \
		$^ \
		-ljim -lm -lssl -lcrypto -lz
	if [ "$$(uname)" = "Darwin" ]; then \
		dsymutil $@; \
	fi

%.o: %.c trie.h
	cc -c -O2 -g -fno-omit-frame-pointer -o$@  \
		-D_GNU_SOURCE $(CFLAGS) $(TRACY_CFLAGS) \
		$< -I./vendor/jimtcl -I./vendor/tracy/public

.PHONY: test clean deps
test: folk
	for test in test/*.folk; do \
		echo "===================="; \
		echo "Running test: $$test"; \
		echo "--------------------"; \
		./folk $$test ; \
	done
clean:
	rm -f folk *.o vendor/tracy/public/TracyClient.o
remote-clean:
	ssh $(FOLK_REMOTE_NODE) -- 'cd folk2; make clean'
deps:
	make -C vendor/jimtcl
	make -C vendor/apriltag libapriltag.so
	if [ "$$(uname)" = "Darwin" ]; then \
		install_name_tool -id @executable_path/vendor/apriltag/libapriltag.so vendor/apriltag/libapriltag.so; \
	fi

FOLK_REMOTE_NODE := folk-live
sync:
	rsync --timeout=5 -e "ssh -o StrictHostKeyChecking=no" --archive \
		--include='**.gitignore' --exclude='/.git' --filter=':- .gitignore' \
		. $(FOLK_REMOTE_NODE):~/folk2 \
		--delete-after
setup-remote:
	ssh-copy-id $(FOLK_REMOTE_NODE)
	make sync
	ssh $(FOLK_REMOTE_NODE) -- 'sudo apt update && sudo apt install libssl-dev gdb libwslay-dev google-perftools libgoogle-perftools-dev linux-perf; cd folk2/vendor/jimtcl; ./configure CFLAGS="-g -fno-omit-frame-pointer"'

remote: sync
	ssh $(FOLK_REMOTE_NODE) -- 'cd folk2; sudo systemctl stop folk; sudo kill -9 `cat folk.pid`; make deps && make CFLAGS=$(CFLAGS) && ./folk'
sudo-remote: sync
	ssh $(FOLK_REMOTE_NODE) -- 'cd folk2; sudo systemctl stop folk; sudo kill -9 `cat folk.pid`; make deps && make CFLAGS=$(CFLAGS) && sudo HOME=/home/folk ./folk'
debug-remote: sync
	ssh $(FOLK_REMOTE_NODE) -- 'cd folk2; sudo systemctl stop folk; sudo kill -9 `cat folk.pid`; make deps && make CFLAGS=$(CFLAGS) && gdb ./folk'
valgrind-remote: sync
	ssh $(FOLK_REMOTE_NODE) -- 'cd folk2; sudo systemctl stop folk; sudo kill -9 `cat folk.pid`; ps aux | grep valgrind | grep -v bash | tr -s " " | cut -d " " -f 2 | xargs kill -9; make deps && make && valgrind --leak-check=yes ./folk'
heapprofile-remote: sync
	ssh $(FOLK_REMOTE_NODE) -- 'cd folk2; sudo systemctl stop folk; sudo kill -9 `cat folk.pid`; make deps && make CFLAGS=$(CFLAGS) && env LD_PRELOAD=libtcmalloc.so HEAPPROFILE=/tmp/folk.hprof PERFTOOLS_VERBOSE=-1 ./folk'
heapprofile-remote-show:
	ssh $(FOLK_REMOTE_NODE) -- 'cd folk2; google-pprof --text folk $(HEAPPROFILE)'
heapprofile-remote-svg:
	ssh $(FOLK_REMOTE_NODE) -- 'cd folk2; google-pprof --svg folk $(HEAPPROFILE)' > out.svg

flamegraph:
	sudo perf record --freq=997 --call-graph lbr --pid=$(shell cat folk.pid) -g -- sleep 30
	sudo perf script -f > out.perf
	~/FlameGraph/stackcollapse-perf.pl out.perf > out.folded
	~/FlameGraph/flamegraph.pl out.folded > out.svg

remote-flamegraph:
	ssh -t $(FOLK_REMOTE_NODE) -- make -C folk2 flamegraph
	scp $(FOLK_REMOTE_NODE):~/folk2/out.svg .
	scp $(FOLK_REMOTE_NODE):~/folk2/out.perf .

run-tracy:
	vendor/tracy/profiler/build/tracy-profiler
