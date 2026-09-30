plugins {
    id("org.jetbrains.kotlin.jvm")
    application
}

kotlin {
    jvmToolchain(17)
}

dependencies {
    implementation("org.jetbrains.kotlinx:kotlinx-serialization-json:1.7.3")
    testImplementation(kotlin("test"))
}

application {
    // `gradlew :protocol:run --args="pair <uri>"` drives a simulated phone.
    mainClass.set("app.feathercast.protocol.PhoneSimKt")
}

tasks.test {
    useJUnitPlatform()
}
