# The board's one microphone, shared by Swifty's speech and webcam mode's USB
# microphone: whoever claims it opens and closes it.
idf_component_register(
    SRCS "src/board_mic.cpp"
    INCLUDE_DIRS "include")
