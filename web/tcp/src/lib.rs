//! Two TCP streams over the raw IPv4 datagrams carried by the iPAQ's PPP link.
use smoltcp::iface::{Config, Interface, SocketHandle, SocketSet};
use smoltcp::phy::{Device, DeviceCapabilities, Medium, RxToken, TxToken};
use smoltcp::socket::tcp::{Socket, SocketBuffer, State};
use smoltcp::time::Instant;
use smoltcp::wire::{HardwareAddress, IpAddress, IpCidr, Ipv4Address};
use std::cell::RefCell;
use std::collections::VecDeque;

const HOST: Ipv4Address = Ipv4Address::new(192, 168, 131, 102);
const DEVICE: Ipv4Address = Ipv4Address::new(192, 168, 131, 201);
const MTU: usize = 1500;
const BUFFER: usize = 64 * 1024 + 32;
const MAX_OUT: usize = 8;
const MAX_IN: usize = 128;

#[derive(Default)]
struct Ppp {
    input: VecDeque<Vec<u8>>,
    output: VecDeque<Vec<u8>>,
}

struct PacketIn(Vec<u8>);
impl RxToken for PacketIn {
    fn consume<R, F: FnOnce(&[u8]) -> R>(self, f: F) -> R {
        f(&self.0)
    }
}

struct PacketOut<'a>(&'a mut VecDeque<Vec<u8>>);
impl TxToken for PacketOut<'_> {
    fn consume<R, F: FnOnce(&mut [u8]) -> R>(self, len: usize, f: F) -> R {
        let mut packet = vec![0; len];
        let result = f(&mut packet);
        self.0.push_back(packet);
        result
    }
}

impl Device for Ppp {
    type RxToken<'a> = PacketIn;
    type TxToken<'a> = PacketOut<'a>;

    fn receive(&mut self, _: Instant) -> Option<(Self::RxToken<'_>, Self::TxToken<'_>)> {
        if self.output.len() == MAX_OUT {
            return None;
        }
        self.input
            .pop_front()
            .map(|packet| (PacketIn(packet), PacketOut(&mut self.output)))
    }

    fn transmit(&mut self, _: Instant) -> Option<Self::TxToken<'_>> {
        (self.output.len() < MAX_OUT).then_some(PacketOut(&mut self.output))
    }

    fn capabilities(&self) -> DeviceCapabilities {
        let mut device = DeviceCapabilities::default();
        device.medium = Medium::Ip;
        device.max_transmission_unit = MTU;
        device
    }
}

struct ActiveSync {
    link: Ppp,
    interface: Interface,
    sockets: SocketSet<'static>,
    dccm: SocketHandle,
    rapi: SocketHandle,
    staging: Box<[u8; BUFFER]>,
}

impl ActiveSync {
    fn new(seed: u32) -> Self {
        let mut link = Ppp::default();
        let mut config = Config::new(HardwareAddress::Ip);
        config.random_seed = u64::from(seed);
        let mut interface = Interface::new(config, &mut link, Instant::from_millis(0));
        interface.update_ip_addrs(|addresses| {
            addresses
                .push(IpCidr::new(IpAddress::from(HOST), 24))
                .unwrap()
        });
        let mut sockets = SocketSet::new(vec![]);
        let mut dccm = tcp_socket(4096);
        dccm.listen(5679).unwrap();
        Self {
            link,
            interface,
            dccm: sockets.add(dccm),
            rapi: sockets.add(tcp_socket(128 * 1024)),
            sockets,
            staging: Box::new([0; BUFFER]),
        }
    }

    fn handle(&self, stream: u32) -> SocketHandle {
        if stream == 0 {
            self.dccm
        } else {
            self.rapi
        }
    }
}

fn tcp_socket(capacity: usize) -> Socket<'static> {
    let mut socket = Socket::new(
        SocketBuffer::new(vec![0; capacity]),
        SocketBuffer::new(vec![0; capacity]),
    );
    socket.set_nagle_enabled(false);
    socket
}

thread_local! {
    static STACK: RefCell<Option<ActiveSync>> = const { RefCell::new(None) };
}

fn with_stack<R>(f: impl FnOnce(&mut ActiveSync) -> R) -> R {
    STACK.with_borrow_mut(|stack| f(stack.as_mut().expect("TCP stack not initialized")))
}

#[no_mangle]
pub extern "C" fn init(seed: u32) -> u32 {
    STACK.with_borrow_mut(|stack| {
        *stack = Some(ActiveSync::new(seed));
        stack.as_mut().unwrap().staging.as_mut_ptr() as u32
    })
}

#[no_mangle]
pub extern "C" fn tick(time_ms: u32) {
    with_stack(|stack| {
        stack.interface.poll(
            Instant::from_millis(i64::from(time_ms)),
            &mut stack.link,
            &mut stack.sockets,
        );
    });
}

#[no_mangle]
pub extern "C" fn receive_packet(length: usize) -> i32 {
    with_stack(|stack| {
        if !(20..=MTU).contains(&length) {
            return 0;
        }
        let packet = &stack.staging[..length];
        if packet[0] >> 4 != 4
            || packet[9] != 6
            || packet[12..16] != DEVICE.octets()
            || packet[16..20] != HOST.octets()
        {
            return 0;
        }
        if stack.link.input.len() >= MAX_IN {
            return -1;
        }
        stack.link.input.push_back(packet.into());
        1
    })
}

#[no_mangle]
pub extern "C" fn next_packet() -> usize {
    with_stack(|stack| {
        if let Some(packet) = stack.link.output.pop_front() {
            stack.staging[..packet.len()].copy_from_slice(&packet);
            packet.len()
        } else {
            0
        }
    })
}

#[no_mangle]
pub extern "C" fn connect_rapi(local_port: u16) -> i32 {
    with_stack(|stack| {
        let socket = stack.sockets.get_mut::<Socket>(stack.rapi);
        socket.abort();
        i32::from(
            socket
                .connect(stack.interface.context(), (DEVICE, 990), local_port)
                .is_ok(),
        )
    })
}

#[no_mangle]
pub extern "C" fn socket_status(stream: u32) -> i32 {
    with_stack(|stack| {
        let socket = stack.sockets.get::<Socket>(stack.handle(stream));
        if socket.can_recv() || socket.state() == State::Established {
            1
        } else if socket.state() == State::Closed || socket.state() == State::CloseWait {
            -1
        } else {
            0
        }
    })
}

#[no_mangle]
pub extern "C" fn socket_read(stream: u32, length: usize) -> i32 {
    with_stack(|stack| {
        let handle = stack.handle(stream);
        let socket = stack.sockets.get_mut::<Socket>(handle);
        socket
            .recv_slice(&mut stack.staging[..length.min(BUFFER)])
            .map_or(-1, |n| n as i32)
    })
}

#[no_mangle]
pub extern "C" fn socket_write(stream: u32, length: usize) -> i32 {
    with_stack(|stack| {
        let handle = stack.handle(stream);
        let socket = stack.sockets.get_mut::<Socket>(handle);
        socket
            .send_slice(&stack.staging[..length.min(BUFFER)])
            .map_or(-1, |n| n as i32)
    })
}
