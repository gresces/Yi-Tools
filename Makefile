TOOLS := yi-niri yi-mail yi-timer

.PHONY: all install uninstall clean test help $(TOOLS)

all: $(TOOLS)

$(TOOLS):
	@$(MAKE) --no-print-directory -C tools/$@ all

install:
	@for tool in $(TOOLS); do \
		$(MAKE) --no-print-directory -C tools/$$tool install || exit 1; \
	done

uninstall:
	@for tool in $(TOOLS); do \
		$(MAKE) --no-print-directory -C tools/$$tool uninstall || exit 1; \
	done

test:
	@for tool in $(TOOLS); do \
		$(MAKE) --no-print-directory -C tools/$$tool test || exit 1; \
	done

clean:
	@for tool in $(TOOLS); do \
		$(MAKE) --no-print-directory -C tools/$$tool clean || exit 1; \
	done

help:
	@echo "make            构建全部工具"
	@echo "make install    安装二进制和 systemd user unit"
	@echo "make test       构建并做基本自检"
	@echo "make clean      删除构建产物"
	@echo "make <tool>     只构建某个工具:$(TOOLS)"
