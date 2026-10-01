# Python interpreter for tools/imgtool.py, shared by the firmware and host
# builds. Prefers the project's virtual environment, which has
# tools/requirements.txt installed.
if(EXISTS ${CMAKE_SOURCE_DIR}/.venv/bin/python)
    set(IMGTOOL_PYTHON_DEFAULT ${CMAKE_SOURCE_DIR}/.venv/bin/python)
else()
    set(IMGTOOL_PYTHON_DEFAULT python3)
endif()
set(IMGTOOL_PYTHON ${IMGTOOL_PYTHON_DEFAULT} CACHE STRING
    "Python interpreter with tools/requirements.txt installed")

set(IMGTOOL ${CMAKE_SOURCE_DIR}/tools/imgtool.py)
