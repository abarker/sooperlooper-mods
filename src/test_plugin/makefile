PYTHON_INCLUDES := $(shell python3-config --includes)

all:
	swig -python -c++ test_engine.swg
	g++ -fPIC -fpermissive -g -shared -o _test_engine.so \
	    test_engine.cpp test_looper.cpp ../plugin.cc test_engine_wrap.cxx \
	    $(PYTHON_INCLUDES) -ljack

clean:
	rm -f _test_engine.so test_engine.py test_engine.pyc test_engine_wrap.cxx
