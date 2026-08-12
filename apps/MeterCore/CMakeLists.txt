# MeterCore is the metering delivery group: the daemons that acquire, project,
# persist, and publish meter data. Protocol gateways (Modbus/BACnet) form their
# own group beside it rather than growing this one.
add_subdirectory(Services)
