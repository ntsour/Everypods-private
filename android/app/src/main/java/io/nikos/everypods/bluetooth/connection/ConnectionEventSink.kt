package io.nikos.everypods.bluetooth.connection

interface ConnectionEventSink {
    fun emit(event: ConnectionEvent)
}
