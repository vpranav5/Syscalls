
extern "C" int sysHandler(uint32_t eax, uint32_t *frame) {
    // get the arguments from user stack and get the user buffer that was pushed from user mode
    // print that buffer's contents but limit it to the number of bytes which is the other parameter user pushed.
    // frame is pointer to the kernel's esp

    // if its 0, then shutdown.
    // frame is a pointer to the top of the kernel stack
   // Debug::printf("In sys handler");
    uint32_t* userESP = (uint32_t*)frame[3];
    // check if the user is sending a valid FD
    // the return address is at esp[0]

    if(eax == 0){ // exit call
        uint32_t exitStatus = userESP[1];
        Future<uint32_t>* sharedFuture = active()-> parentFuture;
        sharedFuture->set(exitStatus);
        stop();
    }
    else if(eax == 1){ // write call
        //# ssize_t read(int fd, void* buffer, size_t n)
        int writeFD = (int)userESP[1];
        void* writeBuffer = (char*)userESP[2];
        size_t writeBytes = (size_t)userESP[3]; // readBytes shouldn't be over file size

        if(writeFD <= 0 || writeFD >= 100){ // don't write to stdin as well, files are in range 0 - 99
            return -1;
        }

        // getting a file
        auto myProcessTable = active()->processTable;
        auto tableValue = myProcessTable[writeFD];

        if(tableValue == nullptr){ // check for openfile
            return -1;
        }

        // confirmed we have an openfile 

        return tableValue->write(writeBuffer, writeBytes);
    }


else if(eax == 2){ // fork
        // fork doesnt take any parameters

        auto myProcessTable = active() -> processTable;
        Thread* parentProc = active();
        Future<uint32_t>* procSync = new Future<uint32_t>(); // use this to let parent wait on child

        int allocateIndex = active() -> findFreeFDIndex(2); // find where to put a child process

        ChildProcess* childProc = new ChildProcess(procSync);

        if(allocateIndex == -1){
            return -1;
        }

        myProcessTable[allocateIndex] = childProc; // this is PID cuz this is what we allocated in parent
        uint32_t userEIP = frame[0];

        Semaphore* sema = new Semaphore(0); // check this

        // create the child's thread using lambda expression
        thread([parentProc, userEIP, userESP, procSync, sema](){ // send procSync pointer by value
            // first we have to copy the parent's files and semas to child

            auto childTable = active()-> processTable;
            active()-> parentFuture = procSync; // set child's future


            for(int i = 0; i < 100; i++){ // refactor to two for loops
                auto parentEntry = parentProc -> processTable[i];
                 // files
                if(parentEntry != nullptr && (parentEntry-> getSize() != -1)){
                    OpenFile* parentOpenFile = (OpenFile*)(parentEntry);
                    Node* childFile = parentOpenFile -> file; // soft copy file and offset
                    int childOffset = parentOpenFile -> offset;
                    OpenFile* childOpenFile = new OpenFile(childOffset, childFile);
                    childTable[i] = childOpenFile;
                }
                else if(parentEntry != nullptr){
                    childTable[i] = parentEntry;
                }
            }

            for(int i = 100; i < 200; i++){ // refactor to two for loops
                auto parentEntry = parentProc -> processTable[i];
                 // semaphore
                if(parentEntry != nullptr){
                    childTable[i] = parentEntry; // copy semaphore pointer from parent to child 
                }
             }


            // copy address space
            AddressSpace* adSpace = parentProc -> addressSpace;
            uint32_t* parentPDPtr = adSpace -> pd;

            // code from p7 destructor 
            for(uint32_t directoryEntry = 0x200; directoryEntry < 0x3C0; directoryEntry ++){ // these bounds are for the shared region, ask if this is right??  0x200 0x3C0
                // check if pde's last bit if u should free
                uint32_t myPDE = parentPDPtr[directoryEntry];
                uint32_t presentBit = myPDE & 0x1; // check if pde is there
                uint32_t myPT = (myPDE >> 12);
                myPT = myPT << 12; // move the PDI back to zero out last 12

                if(presentBit == 1){  // we have a pde

                    for(uint32_t pageEntry = 0; pageEntry < 1024; pageEntry ++){
                         // check if pte's last bit if u should free
                        uint32_t myPTE = ((uint32_t*)myPT)[pageEntry]; // get parent pte
                        uint32_t ptePresentBit = myPTE & 0x1;

                        if(ptePresentBit == 1){ // we have a pte
                            // how do we map the child's frame and virtual address here?
                            uint32_t parentPhysical = myPTE >> 12; // get the parent's physical address by zeroing out last 12 bits of the PTE
                            parentPhysical = parentPhysical << 12;

                            uint32_t va = (directoryEntry << 22) | (pageEntry << 12); // child and parent share va

                            memcpy((void*) va, (void*) parentPhysical, VMM::FRAME_SIZE); // this will invoke page fault handler and set everything
                         }
                    }
                }
            }


            sema -> up(); // we're copying the parent's private region to child and the user stack lives in the private region, call sema up after we're done copying
            switchToUser((uint32_t)userEIP, (uint32_t)userESP, 0); // return 0 from child

        }); // thread end

        sema-> down();
        return allocateIndex; // return child PID

    }

    else if(eax == 3){ // sem
      // loop thru descriptor table, find empty thing, initialize the semaphore object, and return the descriptor
      // go into tcb, find descriptor table

      uint32_t semaValue = userESP[1]; // initial value for the semaphore

      if(semaValue < 0){
          return -1;
      }

      auto currentTable = active() -> processTable;
      auto allocateIndex = active()-> findFreeFDIndex(1); // 2 means its a semaphores

      if(allocateIndex == -1){ // if no empty spot found, return an error saying we can't allocate
         return -1;
      }

      SemaphoreObject* newSema = new SemaphoreObject(semaValue); // new beacuse its a pointer
      currentTable[allocateIndex] = newSema; // found an open spot in table
      return allocateIndex;
    }
    else if(eax == 4){ // up
        uint32_t fdIndex = userESP[1];

        if(fdIndex < 100 || fdIndex >= 200){ // 0 - 99 is files, 100 - 199 is semas, 200 - 299 is procs.
            return -1;
        }

        auto myProcessTable = active()->processTable;
        auto ptEntry = myProcessTable[fdIndex];

        // check if ptEntry is a null ptr, then return -1
        if(ptEntry == nullptr){
          return -1;
        }
        return ptEntry -> up();
    }

    else if(eax == 5){ // down
        uint32_t fdIndex = userESP[1];

        if(fdIndex < 100 || fdIndex >= 200){ // 0 - 99 is files, 100 - 199 is semas, 200 - 299 is procs.
            return -1;
        }

        auto myProcessTable = active()->processTable;
        auto ptEntry = myProcessTable[fdIndex];

        if(ptEntry == nullptr){
           return -1;
        }

        return ptEntry -> down(); // down passes in id for which resource theyre looking for, u have to check the id to make sure its a valid sema
    }
    else if(eax == 6){ // close
        uint32_t fdIndex = userESP[1];
        auto myProcessTable = active()->processTable;

        if(fdIndex < 0 || fdIndex > 299){
            return -1;
        }

        auto ptEntry = myProcessTable[fdIndex];

        if(ptEntry == nullptr){ // return error if there is already nothing in fdt
            return -1;
        }

        myProcessTable[fdIndex] = nullptr; // mark this index as empty
        return 0;
    }

    else if(eax == 7){ // shutdown
        Debug::shutdown();
        return 0;
    }

    else if(eax == 8){ // wait
        uint32_t fdIndex = userESP[1];
        auto myProcessTable = active()->processTable;

        if(fdIndex < 200 || fdIndex > 299){ // wait can only be called on a valid child proc
            return -1;
        }

        auto ptEntry = myProcessTable[fdIndex];

         if(ptEntry == nullptr){ // return error if there is already nothing in fdt
            return -1;
        }

        ChildProcess* childProc = (ChildProcess*)(ptEntry);
        Future<uint32_t>* childVal = childProc-> childFuture;

        uint32_t* userPtr = (uint32_t*)userESP[2];
        *userPtr = childVal->get();

        return 0;
    }

    else if(eax == 9){ // execl

        // # int execl(const char* path, const char* arg0, ....);
        //setup new stack, wipe existing address space, and switch to the elf file u load in

        // find where to load the new program from
        char* filePath = (char*)userESP[1];
        Node* foundNode = BobFS::find(fileSystem, filePath);

        if(foundNode == nullptr){
            return -1;
        }

        if(ELF::verifyElfHeader(foundNode) == -1){ // if verifyHeader returns -1, then we don't have a valid ELF file, if it returns 1, it is valid
            return -1;
        }

        // first arg is file path, last arg is a null ptr, each arg is a string, don't need to store userESP[0]

        int stackIndex = 2; // don't need to store return address and file path in the argv array
        char* strArg = (char*)userESP[stackIndex];
        int numArgs = 0;

        while((char*)userESP[stackIndex]!= nullptr){
            numArgs++;
            stackIndex++;
        }
        // num args now stores the number of string args we need to save

        stackIndex = 2;  // userESP[0] is the reurn address, then file path, then all the string args

        char* heapLocations [numArgs];

        uint32_t argLengths [numArgs];
        uint32_t totalStringLength = 0;
        int heapLocIndex = 0;

        while(heapLocIndex < numArgs){

            strArg = (char*)userESP[stackIndex];

            int stringLength = 1; // count the null terminator
            int charIndex = 0;
            while(strArg[charIndex] != '\0'){
                stringLength++;  // copy the null terminator for each string as well
                charIndex++;
            }

            totalStringLength += stringLength;
            argLengths[heapLocIndex] = stringLength;


            char* heapPtr = (char*) malloc(stringLength);
            memcpy((void*)heapPtr, (void*) strArg, stringLength); // string length stores number of bytes needed for string

            heapLocations[heapLocIndex] = heapPtr; // store all the pointers where args are stored in the heap

            heapLocIndex++;
            stackIndex++;
        }

        // all the arguments from stack index 2 onwards are stored on the heap

        // clear the address space
        AddressSpace* oldSpace = active()-> addressSpace;
        active()-> addressSpace =  new AddressSpace(false);
        active()-> addressSpace -> activate();
        delete oldSpace; // free the old address space memory

        // right after clearing, load the new file, need to do it to here to avoid clearing
        uint32_t stackPtr = ELF::load(foundNode); // load the new program specified by file path

        if(stackPtr == 0){ // make sure elf doesn't return a nullptr for the entry value
            return -1;
        }

        // get length of all the strings and pad that to 4b and then increment ur stackesp based on that
        if((totalStringLength % 4) != 0){ // check if bytes % 4 != 0
                totalStringLength = totalStringLength + 4 - (totalStringLength % 4); // e.g. if bytes is 9, aligned is 9 + 4 - 1 = 12
        }

        uint32_t stackESP = 0xefff0000; // start of the new stack in the new address space

        stackESP -= totalStringLength; // get byte diff?

        char* argv [numArgs];

        int index = numArgs-1;

        while(index >= 0){
            char* heapArgLocation = heapLocations[index]; // get the last argument location

            char* string = heapArgLocation;

            int argLength = argLengths[index];

            stackESP = stackESP - argLength; // subtract to get the starting loc to store arg

            memcpy((void*) stackESP, (void*)string, argLength); // deep copy

            argv[index] = (char*)stackESP;

            index--;
        }

        // store the NULL at end of args, NULL takes up 4B
        stackESP = stackESP - 4;
        uint32_t* nullLocation = (uint32_t*)(stackESP);
        *nullLocation = 0; // same thing as nullptr

        // store the argv, argv is a char**, "a", "b" "c" argv[0] = "a", this is an array of char* for the locations of all the strings on the stack
        stackESP = stackESP - (numArgs * 4);
        memcpy((void*) stackESP, (void*)argv, numArgs * 4); // deep copy

        stackESP = stackESP - 4; // store pointer to where argv array is
        uint32_t* argvLocation = (uint32_t*)(stackESP);
        *argvLocation = stackESP + 4; // same thing as nullptr


        // get starting loc for argc
        stackESP = stackESP - 4;
        uint32_t* argcLocation = (uint32_t*)(stackESP);
        *argcLocation = numArgs;

        // crt0.s pushes the return address and moves the stackESP down , user decides the return address, so we don't need to worry about it

        switchToUser(stackPtr, stackESP, 0);
        return -1;
    }

    else if(eax == 10){ // open, should we handle std in here? 
        char* filePath = (char*)userESP[1]; // ignore flags argument
        auto myProcessTable = active()->processTable;
        auto allocateIndex = active()-> findFreeFDIndex(0); // 0 means its an openfile, should be returning 3, not 0

        //Debug::printf("*** Entering open!!");
        if(allocateIndex == -1){ // if no empty spot found, return an error saying we can't allocate
           return -1;
        }

        Node* foundNode = BobFS::find(fileSystem, filePath);

        if(foundNode == nullptr){
            return -1;
        }

        OpenFile* fileInstance = new OpenFile(0, foundNode);
        myProcessTable[allocateIndex] = fileInstance;
        return allocateIndex;
    }
    else if(eax == 11){ // len
        // use node get size
       uint32_t lenFD = userESP[1];

       if(lenFD < 0 || lenFD >= 100){
           return -1;
       }

       // getting a file
        auto myProcessTable = active()->processTable;
        auto tableValue = myProcessTable[lenFD];

        if(tableValue == nullptr){ // entry shoudn't be null
            return -1;
        }

        return tableValue -> getSize();
    }

     else if(eax == 12){ // read, stdin!!

        //# ssize_t read(int fd, void* buffer, size_t n)

        // how to handle stdin, stdout
        // how to access members of subclasses without defining variables
        // what do do about kernel/kernel

        int readFD = (int)userESP[1];
        void* readBuffer = (void*)userESP[2];
        size_t readBytes = (size_t)userESP[3]; // readBytes shouldn't be over file size

        if(readFD < 0 || readFD >= 100){ // index 100 and up is not files
            return -1;
        }

        // getting a file
        auto myProcessTable = active()->processTable;
        auto tableValue = myProcessTable[readFD];

        if(tableValue == nullptr){ // entry shoudn't be null
            return -1;
        }

        return tableValue -> read(readBuffer, readBytes);
    }
    else if(eax == 13){ // seek
        int seekFD = (int)userESP[1];
        int32_t newOffset = (int32_t)userESP[2]; // signed integer type for offset

         // getting a file
        auto myProcessTable = active()->processTable;

        if(seekFD == -1 || newOffset == -1){
            return -1;
        }
        auto tableValue = myProcessTable[seekFD];

        if(tableValue == nullptr){ // entry shoudn't be null
            return -1;
        }

        // seek on a standard file entry should return -1
        return tableValue-> seek(newOffset);
    }

    return -1; // default return if unexpected behavior occur
}

void SYS::init(void) {
    IDT::trap(48,(uint32_t)sysHandler_,3);
}










